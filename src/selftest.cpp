#include "selftest.h"

#include "core/module.h"
#include "core/scheduler.h"
#include "core/cache.h"
#include "core/setting.h"
#include "gui.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "unload.h"
#if MEMENU_GAME
#include "game/game.h"
#include "game/levels.h"
#include "game/player_state.h"
#if MEMENU_FEATURE_TAS
#include "tas/macro.h"
#include "tas/tas.h"
#endif

#include <ME_SDK.hpp>
#include <cmath>
#endif

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace selftest {
namespace {

using json = nlohmann::ordered_json;

struct Options {
    std::string map = "edge_p";
    int checkpoint = 2;         // 0/1 = start of the level: the Prologue's intro cutscene holds the player there
    unsigned ticks = 10;        // Engine ticks (and frames) each module stays on
    std::set<std::string> only, skip;
    bool quit = true;
};

struct Step {
    std::string name;
    bool ok = true;
    std::vector<std::string> problems, notes;
    double seconds = 0.0;
};

bool g_requested = false;
Options g_options;
std::vector<Step> g_steps;
std::string g_current;          // Step in progress (written to the result, so a crash shows where it was)
ULONGLONG g_started = 0;
bool g_aborted = false;         // Unload requested in the middle: stop testing

// Buttons that would open windows on the desktop, delete files or run for minutes: never pressed
const std::set<std::string> kNoPress = {"Open log file", "Open themes folder", "Bake all levels",
                                        "Clear this level's cache",
                                        "Clear saved state"};   // Load State is tested next and needs it

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::set<std::string> SplitList(const std::string& s)
{
    std::set<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (auto t = Trim(item); !t.empty()) out.insert(t);
    return out;
}

std::filesystem::path ResultPath() { return paths::Dir() / "selftest-result.json"; }

void WriteResult(bool finished)
{
    json root;
    root["finished"] = finished;
    bool passed = !g_steps.empty();
    for (const Step& s : g_steps) passed &= s.ok;
    root["passed"] = finished && passed;
    root["current"] = g_current;
    root["seconds"] = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
    json steps = json::array();
    for (const Step& s : g_steps) {
        json j;
        j["name"] = s.name;
        j["ok"] = s.ok;
        j["seconds"] = s.seconds;
        j["problems"] = s.problems;
        j["notes"] = s.notes;
        steps.push_back(std::move(j));
    }
    root["steps"] = std::move(steps);
    const auto path = ResultPath();
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << root.dump(2);
    }
    MoveFileExA(tmp.c_str(), path.string().c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---- Waiting --------------------------------------------------------------------------------------------------------
// Sleeps in small steps; false if unload was requested meanwhile (the test stops then).
bool Wait(double seconds)
{
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (GetTickCount64() < end) {
        if (unload::Requested()) { g_aborted = true; return false; }
        Sleep(1);
    }
    return true;
}

// Waits until `cond` holds or `seconds` pass. True if it held.
bool WaitFor(double seconds, const std::function<bool()>& cond)
{
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (GetTickCount64() < end) {
        if (cond()) return true;
        if (unload::Requested()) { g_aborted = true; return false; }
        Sleep(1);
    }
    return cond();
}

#if MEMENU_GAME
unsigned long long Ticks() { return game::GetStatus().ticks; }
#endif

// Waits until `n` more frames are drawn and the game ticked `n` more times (the slower of the two decides; ticks don't
// count while a level loads). Waiting for ticks and frames instead of seconds keeps the test short on a fast machine
// and fair on a slow one. A stall of `timeout` seconds is logged as an error, which fails the step it happens in.
bool WaitTicks(unsigned n, double timeout = 10.0)
{
    const unsigned frames0 = hooks::EndSceneCalls();
#if MEMENU_GAME
    const auto ticks0 = Ticks();
    const auto done = [&] {
        const bool ticked = !game::Ready() || game::IsLoading() || Ticks() >= ticks0 + n;
        return hooks::EndSceneCalls() >= frames0 + n && ticked;
    };
#else
    const auto done = [&] { return hooks::EndSceneCalls() >= frames0 + n; };
#endif
    if (WaitFor(timeout, done) || g_aborted) return true;
    spdlog::error("Stall: {} frames and ticks did not come within {:.0f} s (frames {} -> {})", n, timeout, frames0,
                  hooks::EndSceneCalls());
    return false;
}

// Work handed to another thread that may outlive the wait for it: callers pass closures that capture their own stack
// by reference, so a job the wait gave up on (a level load stops the game thread's queue) must never run later.
// Abandoning takes the lock, so a job is either finished before its wait returns or never runs at all.
struct Job {
    std::mutex mutex;
    bool done = false, abandoned = false;
};

template <class Post>
bool RunAndWait(Post&& post, std::function<void()> fn, double timeout)
{
    auto job = std::make_shared<Job>();
    post([job, fn = std::move(fn)] {
        std::lock_guard lock(job->mutex);
        if (job->abandoned) return;
        fn();
        job->done = true;
    });
    if (WaitFor(timeout, [&] { std::lock_guard lock(job->mutex); return job->done; })) return true;
    std::lock_guard lock(job->mutex);
    job->abandoned = !job->done;
    return job->done;
}

// Runs `fn` on the render thread and waits for it (like a click in the menu). False if no frame came to run it.
bool OnRenderThread(std::function<void()> fn, double timeout = 5.0)
{
    return RunAndWait([](std::function<void()> f) { hooks::RunOnRenderThread(std::move(f)); }, std::move(fn), timeout);
}

#if MEMENU_GAME
std::atomic<int> g_loadsFinished{0};
void OnLevelLoad(bool finished) { if (finished) ++g_loadsFinished; }

// Waits for a level load that may be running (or about to start) to finish and the game to tick again.
bool SettleAfterLoad(double seconds)
{
    if (!WaitFor(seconds, [] { return !game::IsLoading(); })) return false;
    const auto t = Ticks();
    return WaitFor(seconds, [&] { return Ticks() > t + 5; });
}

// Runs `fn` on the game thread (in the next engine tick) and waits for it. False if the game didn't tick.
bool OnGameThread(std::function<void()> fn, double timeout = 5.0)
{
    return RunAndWait([](std::function<void()> f) { game::RunOnGameThread(std::move(f)); }, std::move(fn), timeout);
}

// Evaluates `check` on the game thread: "" when it holds, else what is wrong.
std::string Verify(const std::function<std::string()>& check)
{
    std::string result = "the game thread did not run the check";
    OnGameThread([&] { result = check(); });
    return result;
}

using Classes::ATdPlayerController;
using Classes::ATdPlayerPawn;

// The player, for checks: both or a problem text.
template <class F>
std::string WithPlayer(F&& f)
{
    ATdPlayerPawn* pawn = game::Pawn();
    ATdPlayerController* pc = game::Controller();
    if (!pawn || !pc) return "no player to check";
    return f(pawn, pc);
}

std::string Expect(bool ok, const char* what) { return ok ? std::string() : std::string(what); }

float Distance(const Classes::FVector& a, const Classes::FVector& b)
{
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Classes::FVector g_savedAt{};   // Where Save State was taken (Load State must come back to it)

// What a module must do to the game, beyond not breaking anything. Each part optional:
//   setup     game thread, before it is switched on / triggered
//   whileOn   game thread, half way through (actions: after the trigger)
//   afterOff  game thread, after it is switched off again
struct Expectation {
    std::function<void()> setup;
    std::function<std::string()> whileOn, afterOff;
};

Expectation ExpectationFor(Module& m)
{
    const std::string& name = m.Name();
    if (name == "God Mode")
        return {{},
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) {
                    return Expect(c->bGodMode && p->Health == p->MaxHealth, "bGodMode not set or health not full"); }); },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(!c->bGodMode, "bGodMode still set after switching off"); }); }};
    if (name == "No Clip")
        return {{},
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(!p->bCollideWorld && p->Physics == Classes::EPhysics::PHYS_None,
                                  "the player still collides or has physics"); }); },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(p->bCollideWorld, "collision not back on after switching off"); }); }};
    if (name == "Flight")
        return {{},
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(p->Physics == Classes::EPhysics::PHYS_None, "the player still has physics"); }); },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(p->Physics != Classes::EPhysics::PHYS_None, "physics not back after switching off"); }); }};
    if (name == "Game Speed") {
        auto* slider = dynamic_cast<ctl::Slider*>(m.FindSetting("Multiplier"));
        return {[slider] { if (slider) slider->Set(0.5f); },
                [] { auto* w = game::World();
                     return Expect(w && std::fabs(w->TimeDilation - 0.5f) < 0.01f, "TimeDilation is not the multiplier (0.5)"); },
                [] { auto* w = game::World();
                     return Expect(w && std::fabs(w->TimeDilation - 1.f) < 0.01f, "TimeDilation not back to 1"); }};
    }
    if (name == "Save State")
        return {[] { player_state::Clear(); if (auto* p = game::Pawn()) g_savedAt = p->Location; },
                [] { return Expect(player_state::HasSave(), "no state was saved"); }, {}};
    if (name == "Load State")
        return {[] { if (auto* p = game::Pawn()) { p->Location.X += 2000.f; p->Velocity = Classes::FVector{}; } },   // Move away
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    const float d = Distance(p->Location, g_savedAt);
                    return d < 150.f ? std::string() : "the player is " + std::to_string(static_cast<int>(d)) +
                                                       " units from the saved position after loading it"; }); },
                {}};
    if (name == "Refill Reaction Time")
        return {[] { if (auto* c = game::Controller()) c->ReactionTimeEnergy = 0.f; },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(c->ReactionTimeEnergy >= 99.f, "the reaction time meter was not refilled"); }); },
                {}};
    if (name == "FOV") {
        auto* slider = dynamic_cast<ctl::Slider*>(m.FindSetting("Degrees"));
        return {[slider] { if (slider) slider->Set(110.f); },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    if (!c->PlayerCamera) return std::string("no player camera");
                    const float fov = c->PlayerCamera->GetFOVAngle();   // What the view is rendered with
                    return std::fabs(fov - 110.f) < 0.5f ? std::string()
                                                        : "the camera renders at " + std::to_string(fov) + " degrees, not 110"; }); },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(c->PlayerCamera && !c->PlayerCamera->bLockedFOV, "the camera's FOV is still locked"); }); }};
    }
    if (name == "Teleport to Spawn")
        return {[] { if (auto* p = game::Pawn()) { p->Location.Z += 600.f; p->Location.X += 300.f; g_savedAt = p->Location; } },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    char where[160];
                    std::snprintf(where, sizeof where, "the player was not moved: at (%.0f, %.0f, %.0f), moved away to (%.0f, %.0f, %.0f)",
                                  p->Location.X, p->Location.Y, p->Location.Z, g_savedAt.X, g_savedAt.Y, g_savedAt.Z);
                    return Distance(p->Location, g_savedAt) > 200.f ? std::string() : std::string(where); }); },
                {}};
    if (name == "Levels")
        return {{}, [] { return Expect(!levels::Maps().empty(), "the level list is empty"); }, {}};
    if (name == "Sublevels")
        return {{}, [] { return Expect(!levels::Sublevels().empty(), "no streaming sublevels listed for this level"); }, {}};
    return {};
}
#endif

// ---- Checks around a step -------------------------------------------------------------------------------------------
// Records the frame/tick counters and the log position when a step starts, and checks them when it ends.
class Probe {
public:
    explicit Probe(Step& step) : step_(step)
    {
        frames_ = hooks::EndSceneCalls();
#if MEMENU_GAME
        ticks_ = Ticks();
#endif
        std::vector<logging::Entry> ignored;
        logId_ = logging::CopySince(0, ignored);        // Newest id
        start_ = GetTickCount64();
    }

    // `minFrames` frames must have been drawn, the game must have ticked (unless a level load may have stopped it),
    // and nothing may have logged an error. Warnings become notes.
    void Check(unsigned minFrames, bool expectTicks)
    {
        const unsigned frames = hooks::EndSceneCalls() - frames_;
        if (frames < minFrames)
            Problem("only " + std::to_string(frames) + " frames drawn (expected at least " + std::to_string(minFrames) +
                    "): rendering stalled");
#if MEMENU_GAME
        const auto ticks = Ticks() - ticks_;
        if (expectTicks && game::Ready() && !game::IsLoading() && ticks == 0)
            Problem("the game did not tick: game thread stalled");
#else
        (void)expectTicks;
#endif
        std::vector<logging::Entry> entries;
        logging::CopySince(logId_, entries);
        for (const auto& e : entries) {
            if (e.text.rfind("Self-test", 0) == 0) continue;
            if (e.level >= logging::Level::Error) Problem("log: " + e.text);
            else if (e.level == logging::Level::Warn) step_.notes.push_back("log warning: " + e.text);
        }
        step_.seconds = static_cast<double>(GetTickCount64() - start_) / 1000.0;
    }

    void Problem(std::string text)
    {
        spdlog::error("Self-test: {}: {}", step_.name, text);
        step_.ok = false;
        step_.problems.push_back(std::move(text));
    }

private:
    Step& step_;
    unsigned frames_ = 0;
    unsigned long long ticks_ = 0;
    std::uint64_t logId_ = 0;
    ULONGLONG start_ = 0;
};

Step& Begin(std::string name)
{
    spdlog::info("Self-test: {}...", name);
    g_current = name;
    g_steps.push_back({std::move(name)});
    WriteResult(false);
    return g_steps.back();
}

void End(Step& s)
{
    spdlog::info("Self-test: {} {}{}", s.name, s.ok ? "ok" : "FAILED", s.problems.empty() ? "" : ": " + s.problems.front());
    g_current.clear();
    WriteResult(false);
}

// ---- Steps ----------------------------------------------------------------------------------------------------------
bool TestFrames()
{
    Step& s = Begin("frames");
    const bool drew = WaitFor(30.0, [] { return hooks::EndSceneCalls() > 0; });
    if (!drew) {
        Probe(s).Problem("EndScene never ran in 30 s: the menu can't draw");
        End(s);
        return false;
    }
    Probe p(s);
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(10);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(5);
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(5);
    p.Check(20, false);
    End(s);
    return s.ok;
}

#if MEMENU_GAME
bool TestGame()
{
    Step& s = Begin("game");
    Probe p(s);
    if (!game::Ready()) p.Problem("game integration not ready (patterns or hooks failed, see the log)");
    else if (!WaitFor(10.0, [t = Ticks()] { return Ticks() > t + 10; })) p.Problem("the engine tick hook does not run");
    p.Check(1, false);
    End(s);
    return s.ok;
}

bool TestLevel()
{
    if (Lower(g_options.map) == "none") return true;
    Step& s = Begin("level " + g_options.map);
    Probe p(s);
    // The list fills from the game's own data a moment after startup
    std::vector<levels::MapView> maps;
    WaitFor(15.0, [&] { maps = levels::Maps(); return !maps.empty(); });
    const std::string want = Lower(g_options.map);
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(want) != std::string::npos) { index = i; break; }
    if (index < 0) {
        p.Problem("no level matching '" + g_options.map + "' in the level list (" + std::to_string(maps.size()) + " levels)");
        End(s);
        return false;
    }
    const int checkpoint = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));
    s.notes.push_back("loading " + maps[index].label +
                      (checkpoint > 0 ? ", checkpoint " + maps[index].checkpoints[checkpoint - 1] : ", level start"));
    const int before = g_loadsFinished.load();
    OnRenderThread([index, checkpoint] { levels::Load(index, checkpoint, levels::Method::Game, false); });
    if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > before; }))
        p.Problem("the level did not finish loading within 120 s");
    else if (!WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); }))
        p.Problem("no player pawn 60 s after the level loaded");
    else {
        WaitTicks(30);   // Let streaming settle
        // A cutscene holds the player in place: nothing that moves them could be checked
        bool inCutscene = true;
        WaitFor(30.0, [&] {
            OnGameThread([&] { auto* pawn = game::Pawn(); inCutscene = !pawn || pawn->bCutsceneIsSkippable; }, 1.0);
            return !inCutscene;
        });
        if (inCutscene) {
            bool cinematic = false;
            OnGameThread([&] { if (auto* c = game::Controller()) cinematic = c->bCinematicMode != 0; });
            p.Problem(std::string("the player is still in a cutscene 30 s after loading (pick another checkpoint); "
                                  "controller cinematic mode: ") + (cinematic ? "on" : "off"));
        }
    }
    s.notes.push_back(levels::LastMessage());
    p.Check(1, true);
    End(s);
    return s.ok;
}

// While the menu is open the game must not turn the camera (bIgnoreLookInput), and must get it back when it closes.
void TestInputBlocking()
{
    Step& s = Begin("input blocking");
    Probe p(s);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(3);
    bool before = false;
    OnGameThread([&] { if (auto* c = game::Controller()) before = c->bIgnoreLookInput != 0; });
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(3);
    if (const auto problem = Verify([] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
            return Expect(c->bIgnoreLookInput, "look input not blocked while the menu is open"); }); });
        !problem.empty())
        p.Problem(problem);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(3);
    if (const auto problem = Verify([before] { return WithPlayer([before](ATdPlayerPawn*, ATdPlayerController* c) {
            return Expect((c->bIgnoreLookInput != 0) == before, "look input not given back when the menu closed"); }); });
        !problem.empty())
        p.Problem(problem);
    OnRenderThread([] { gui::visible = true; });
    p.Check(1, true);
    End(s);
}

#if MEMENU_FEATURE_TAS
// The player's own key for a binding command ("GBA_MoveForward"): keyboard keys only, no modifier combos. Game thread.
std::string KeyFor(ATdPlayerController* pc, const std::string& command, const char* fallback)
{
    if (pc && pc->PlayerInput) {
        auto& binds = pc->PlayerInput->Bindings;
        for (std::size_t i = 0; i < binds.Num(); ++i) {
            const auto& b = binds[i];
            if (b.Control || b.Shift || b.Alt || !b.Command.IsValid()) continue;
            const std::string cmd = b.Command.ToString(), key = b.Name.GetName();
            if (key.rfind("XboxTypeS_", 0) == 0 || key.rfind("GBA_", 0) == 0) continue;   // Gamepad, or an alias
            if (cmd.rfind(command, 0) == 0 && (cmd.size() == command.size() || cmd[command.size()] == ' ' ||
                                               cmd[command.size()] == '|'))
                return key;
        }
    }
    return fallback;
}

// A macro played through the TAS: the player's forward key held for a second with a jump half way, written with
// tas::Macro instead of recorded. Proves the whole playback path in the game: the replay's key names resolve, the
// inputs reach the player through the engine, every frame runs, and the keys are let go at the end.
void TestTasMacro()
{
    Step& s = Begin("TAS macro");
    Probe p(s);
    if (!tas::Available()) {
        p.Problem("the TAS engine hooks were not found");
        p.Check(1, false);
        End(s);
        return;
    }

    struct Pose { Classes::FVector at{}; int yaw = 0; bool ok = false; };
    std::string forward, jump;
    Pose start;
    OnGameThread([&] {
        auto* pc = game::Controller();
        forward = KeyFor(pc, "GBA_MoveForward", "W");
        jump = KeyFor(pc, "GBA_Jump", "SpaceBar");
        if (auto* pawn = game::Pawn(); pawn && pc) {
            start = {pawn->Location, static_cast<int>(pc->Rotation.Yaw), true};
            player_state::Save();   // Put back afterwards: running on a rooftop can end badly
        }
    });
    if (!start.ok) {
        p.Problem("no player to drive");
        p.Check(1, false);
        End(s);
        return;
    }
    s.notes.push_back("keys: forward " + forward + ", jump " + jump);

    tas::Macro m(60.f);
    m.Describe("memenu self-test: run forward, jump").Wait(5).Press(forward).Wait(30).Hold(jump, 2).Wait(28)
        .Release(forward).Wait(20);
    const tas::Replay macro = m.Build();

    tas::Replay saved;   // Whatever the user has loaded in the TAS editor
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([] { gui::visible = false; });
    tas::Play();

    // Follow it: the highest point the player reaches. Playback paces itself at the replay's 60 fps.
    float topZ = start.at.Z;
    bool airborne = false, started = false;
    tas::Status st{};
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline && !g_aborted) {
        st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame >= st.frames)) break;   // Done (or paused at the end)
        OnGameThread([&] {
            if (auto* pawn = game::Pawn()) {
                topZ = std::max(topZ, pawn->Location.Z);
                airborne |= pawn->Physics == Classes::EPhysics::PHYS_Falling;
            }
        }, 0.5);
    }
    const std::size_t reached = st.frame, frames = macro.FrameCount();
    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");
    WaitTicks(30);   // Let the player come to a stop

    if (!started) p.Problem("playback never started: " + tas::GetStatus().message);
    else if (reached < frames)
        p.Problem("playback stopped at frame " + std::to_string(reached) + " of " + std::to_string(frames) + ": " +
                  tas::GetStatus().message);

    // Ran the way they were facing (Unreal yaw: 65536 = a full turn), left the ground, and stopped again
    const std::string moved = Verify([&] {
        return WithPlayer([&](ATdPlayerPawn* pawn, ATdPlayerController*) {
            const double yaw = start.yaw * (6.283185307179586 / 65536.0);
            const double dx = pawn->Location.X - start.at.X, dy = pawn->Location.Y - start.at.Y;
            const double along = dx * std::cos(yaw) + dy * std::sin(yaw), dist = std::sqrt(dx * dx + dy * dy);
            const double speed = std::sqrt(pawn->Velocity.X * pawn->Velocity.X + pawn->Velocity.Y * pawn->Velocity.Y);
            char info[200];
            std::snprintf(info, sizeof info, "moved %.0f units (%.0f forward), %.0f units up at most, speed after %.0f",
                          dist, along, topZ - start.at.Z, speed);
            std::string problems;
            if (along < 150.0 || along < 0.7 * dist) problems += "did not run forward; ";
            if (!airborne && topZ - start.at.Z < 30.f) problems += "never jumped; ";
            if (speed > 100.0) problems += "still running after the macro released the key; ";
            return problems.empty() ? std::string("ok: ") + info : problems + info;
        });
    });
    if (moved.rfind("ok: ", 0) == 0) s.notes.push_back(moved.substr(4));
    else p.Problem(moved);

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnGameThread([] { player_state::Load(); });
    WaitTicks(5);
    OnRenderThread([] { gui::visible = true; });
    p.Check(1, true);
    End(s);
}
#endif
#endif

// Setting values of every module, to put back after the test
struct Saved {
    bool enabled;
    int key;
    json settings;
    std::vector<std::pair<int, int>> keys;
};

std::map<Module*, Saved> SaveAll()
{
    std::map<Module*, Saved> out;
    modules::ForEach([&](modules::Tab&, Module& m) {
        Saved sv{m.Enabled(), m.key, json::object(), {}};
        for (ctl::Setting* st : m.Settings()) {
            sv.keys.emplace_back(st->key, st->keyAlt);
            if (!st->Persistent()) continue;
            json v;
            st->Save(v);
            sv.settings[st->Name()] = v;
        }
        out.emplace(&m, std::move(sv));
    });
    return out;
}

void RestoreAll(const std::map<Module*, Saved>& saved)
{
    OnRenderThread([&] {
        for (const auto& [m, sv] : saved) {
            std::size_t i = 0;
            for (ctl::Setting* st : m->Settings()) {
                if (i < sv.keys.size()) { st->key = sv.keys[i].first; st->keyAlt = sv.keys[i].second; }
                ++i;
                if (st->Persistent() && sv.settings.contains(st->Name())) st->Load(sv.settings[st->Name()]);
            }
            m->key = sv.key;
            if (m->Enabled() != sv.enabled) m->SetEnabled(sv.enabled);
            m->expanded = false;
        }
    }, 10.0);
}

bool Selected(const Module& m)
{
    if (!g_options.only.empty() && !g_options.only.count(m.Name())) return false;
    return !g_options.skip.count(m.Name());
}

const sched::EntryView* FindEntry(const std::vector<sched::EntryView>& entries, const std::string& name)
{
    for (const auto& e : entries) if (e.name == name) return &e;
    return nullptr;
}

void TestModule(Module& m)
{
    Step& s = Begin("module " + m.Tab() + "/" + m.Name());
    const unsigned n = g_options.ticks;
    Probe p(s);
    const auto runsBefore = [&] { const auto e = sched::Snapshot(); const auto* v = FindEntry(e, m.Name()); return v ? v->runs : 0; }();
#if MEMENU_GAME
    const Expectation expect = ExpectationFor(m);
    // Main Menu loads a level; Restart Checkpoint respawns the player (a new pawn, usually without a level load)
    const bool loadsLevel = m.Name() == "Main Menu";
    const bool respawns = m.Name() == "Restart Checkpoint";
    const int loadsBefore = g_loadsFinished.load();
    const void* pawnBefore = nullptr;
    if (respawns) OnGameThread([&] { pawnBefore = game::Pawn(); });
    if (expect.setup) OnGameThread(expect.setup);
#endif

    if (m.IsAction()) {
        OnRenderThread([&m] { m.expanded = true; m.Toggle(); });
    } else {
        bool switchedOn = false;   // Read right away: one-shot tools (Kickglitch, ...) may fire and switch off next tick
        OnRenderThread([&m, &switchedOn] {
            m.expanded = true;
            if (m.Enabled()) m.SetEnabled(false);   // On by default: test switching it on too
            m.SetEnabled(true);
            switchedOn = m.Enabled();
        });
        if (!switchedOn) p.Problem("did not switch on");
    }
    WaitTicks(n);
#if MEMENU_GAME
    if (expect.whileOn && !loadsLevel)
        if (const std::string problem = Verify(expect.whileOn); !problem.empty()) p.Problem(problem);
#endif

    // Every control that can be bound to a key, pressed once per slot (what a key press does)
    OnRenderThread([&m] {
        for (ctl::Setting* st : m.Settings()) {
            if (kNoPress.count(st->Name())) continue;
            for (int slot = 0; slot < 2; ++slot)
                if (st->BindLabel(slot)) st->Press(slot);
        }
    });
    WaitTicks(std::max(2u, n / 2));

    OnRenderThread([] { gui::visible = false; });   // HUD-only drawing (some modules draw differently)
    WaitTicks(std::max(2u, n / 2));
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(2);

#if MEMENU_GAME
    // Actions may start a level load (Restart Checkpoint, Main Menu): wait for it before judging the game thread
    if (loadsLevel) WaitFor(30.0, [&] { return g_loadsFinished.load() > loadsBefore; });
    if (game::IsLoading() || g_loadsFinished.load() > loadsBefore) SettleAfterLoad(120.0);
    if (loadsLevel && g_loadsFinished.load() <= loadsBefore) p.Problem("no level load happened");
    if (respawns) {
        const void* pawnAfter = nullptr;
        WaitFor(30.0, [&] {
            OnGameThread([&] { pawnAfter = game::Pawn(); }, 1.0);
            return pawnAfter && (pawnAfter != pawnBefore || g_loadsFinished.load() > loadsBefore);
        });
        if (!pawnAfter) p.Problem("no player 30 s after restarting");
        else if (pawnAfter == pawnBefore && g_loadsFinished.load() <= loadsBefore) p.Problem("the player was not respawned");
    }
#endif

    const auto entries = sched::Snapshot();
    if (const auto* e = FindEntry(entries, m.Name())) {
        if (e->state == sched::State::Failed) p.Problem("the scheduler marked it failed (its OnTick threw)");
        if (!m.IsAction() && m.Enabled())
            s.notes.push_back("OnTick ran " + std::to_string(e->runs - runsBefore) + " times, avg " +
                              std::to_string(e->avgUs) + " us; OnRender avg " + std::to_string(e->renderAvgUs) + " us");
    }

    if (!m.IsAction()) {
        OnRenderThread([&m] { m.SetEnabled(false); });
        if (m.Enabled()) p.Problem("did not switch off");
    }
    OnRenderThread([&m] { m.expanded = false; });
    WaitTicks(3);   // OnDisable hands its cleanup to the next tick
#if MEMENU_GAME
    if (expect.afterOff)
        if (const std::string problem = Verify(expect.afterOff); !problem.empty()) p.Problem("after off: " + problem);
#endif
    p.Check(n, !m.IsAction());
    End(s);
}

void TestAllOn(const std::vector<Module*>& mods)
{
    Step& s = Begin("all modules on");
    Probe p(s);
    OnRenderThread([&] {
        for (Module* m : mods) if (!m->IsAction()) m->SetEnabled(true);   // Groups switch each other off
    });
    WaitTicks(g_options.ticks * 3);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(g_options.ticks);
    OnRenderThread([] { gui::visible = true; });
    for (const auto& e : sched::Snapshot())
        if (e.state == sched::State::Failed) p.Problem(e.name + ": the scheduler marked it failed");
    OnRenderThread([&] { for (Module* m : mods) if (!m->IsAction()) m->SetEnabled(false); });
    WaitTicks(3);
    p.Check(g_options.ticks * 4, true);
    End(s);
}

void Performance()
{
    Step& s = Begin("performance");
    const auto t = sched::Ticks();
    char line[200];
    std::snprintf(line, sizeof line, "menu work per tick: avg %.3f ms, last %.3f ms, budget %.2f ms, pressure x%.2f, %llu ticks",
                  t.avgMs, t.lastMs, t.budgetMs, t.pressure, static_cast<unsigned long long>(t.ticks));
    s.notes.push_back(line);
    for (const auto& e : sched::Snapshot()) {
        if (e.maxUs < 1000.f && e.renderMaxUs < 1000.f) continue;
        std::snprintf(line, sizeof line, "%s: tick avg %.0f us max %.0f us, render avg %.0f us max %.0f us", e.name.c_str(),
                      e.avgUs, e.maxUs, e.renderAvgUs, e.renderMaxUs);
        s.notes.push_back(line);
    }
    for (const auto& c : cache::Snapshot()) {
        std::snprintf(line, sizeof line, "cache %s (%s): %llu hits, %llu misses, %.1f ms computing", c.name.c_str(),
                      cache::ScopeName(c.scope), static_cast<unsigned long long>(c.hits),
                      static_cast<unsigned long long>(c.misses), c.computeSeconds * 1000.0);
        s.notes.push_back(line);
    }
    End(s);
}

void ReadOptions(std::istream& in)
{
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq))), value = Trim(line.substr(eq + 1));
        if (key == "map") g_options.map = value;
        else if (key == "checkpoint") g_options.checkpoint = std::max(0, std::atoi(value.c_str()));
        else if (key == "ticks") g_options.ticks = static_cast<unsigned>(std::clamp(std::atoi(value.c_str()), 2, 3600));
        else if (key == "only") g_options.only = SplitList(value);
        else if (key == "skip") g_options.skip = SplitList(value);
        else if (key == "quit") g_options.quit = value != "0";
        else spdlog::warn("Self-test: unknown option '{}'", key);
    }
}

BOOL CALLBACK CloseGameWindow(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

} // namespace

bool Requested()
{
    const auto path = paths::Dir() / "selftest";
    std::ifstream in(path);
    if (!in) return false;
    ReadOptions(in);
    in.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);   // Once: a crash must not make every later start a test
    std::filesystem::remove(ResultPath(), ec);
    g_requested = true;
    g_started = GetTickCount64();
    spdlog::info("Self-test requested: map={} checkpoint={} ticks={} quit={}", g_options.map, g_options.checkpoint,
                 g_options.ticks, g_options.quit);
    WriteResult(false);
    return true;
}

void Run()
{
    if (!g_requested) return;
#if MEMENU_GAME
    game::OnLevelLoad(&OnLevelLoad);
#endif
    const auto saved = SaveAll();

    if (TestFrames()) {
#if MEMENU_GAME
        if (TestGame() && !g_aborted && TestLevel() && !g_aborted && Lower(g_options.map) != "none") {
            TestInputBlocking();
#if MEMENU_FEATURE_TAS
            const bool tasWanted = (g_options.only.empty() || g_options.only.count("TAS macro")) &&
                                   !g_options.skip.count("TAS macro");
            if (tasWanted && !g_aborted) TestTasMacro();
#endif
        }
#endif
        std::vector<Module*> mods;
        modules::ForEach([&](modules::Tab&, Module& m) { if (Selected(m)) mods.push_back(&m); });
        // Actions that leave the level go last
        std::stable_partition(mods.begin(), mods.end(), [](Module* m) { return m->Name() != "Main Menu"; });
        for (Module* m : mods) {
            if (g_aborted) break;
            TestModule(*m);
        }
        if (!g_aborted && g_options.only.empty()) TestAllOn(mods);
    }
    if (!g_aborted) Performance();
    RestoreAll(saved);
    gui::visible = false;
    spdlog::info("Self-test: done, unloading");
}

void Finish(bool unloadedCleanly)
{
    if (!g_requested) return;
    Step& s = Begin("unload");
    if (!unloadedCleanly) {
        s.ok = false;
        s.problems.push_back("hooks were not removed cleanly");
    }
    End(s);
    if (g_aborted) {
        Step& a = Begin("aborted");
        a.ok = false;
        a.problems.push_back("unload was requested in the middle of the test (END, or the unload event)");
        End(a);
    }
    WriteResult(true);
    bool passed = true;
    for (const Step& st : g_steps) passed &= st.ok;
    spdlog::info("Self-test {}: {} steps", passed ? "PASSED" : "FAILED", g_steps.size());
    if (g_options.quit) {
        spdlog::info("Self-test: closing the game");
        EnumWindows(CloseGameWindow, 0);
    }
}

} // namespace selftest
