#include "selftest.h"

#include "core/module.h"
#include "core/notify.h"
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
#include "game/uobject.h"
#include "game/levels.h"
#include "game/player_state.h"
#if MEMENU_FEATURE_SPEEDRUN_TIMER
#include "speedrun/live.h"
#endif
#if MEMENU_FEATURE_COLLISION_VIEW
#include "game/debug_world.h"
#endif
#if MEMENU_FEATURE_TAS
#include "features/tas/timeline_view.h"
#include "tas/input_monitor.h"
#include "tas/macro.h"
#include "tas/timeline.h"
#include "tas/tas.h"
#endif

#include <ME_SDK.hpp>
#include <cmath>
#include <cstdio>
#endif

#include <Windows.h>
#include <nlohmann/json.hpp>
#include "game/object_ref.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
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
    // The engine's frame rate cap (62) off while testing, and TAS playbacks that don't depend on pacing as fast as they
    // go: the waits count frames and ticks, so they pass as fast as the PC draws. Off: the game as players run it.
    bool fast = true;
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
const std::set<std::string> kNoPress = {"Open log file", "Open themes folder",
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

// Why the player can't be moved right now ("" when they can): no pawn, a cutscene, input held back by the level's
// scripting, frozen. After a load the game holds the player for a moment; a count of ticks is no measure of that (with
// the frame rate cap off, 30 ticks pass in a tenth of a second), this is.
std::string PlayerNotReady()
{
    std::string why = "the game thread did not answer";
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) why = "no player";
        else if (pawn->bCutsceneIsSkippable || pc->bCinematicMode) why = "in a cutscene";
        else if (pc->bIgnoreMoveInput) why = "the level holds back the player's input";
        else if (pc->bFrozen) why = "the player is frozen";
        else why.clear();
    }, 1.0);
    return why;
}

// Waits until the player can be moved, then a quarter second more (the level's own scripts settle). The reason when not.
std::string WaitPlayerReady(double timeout = 30.0)
{
    std::string why;
    if (!WaitFor(timeout, [&] { why = PlayerNotReady(); return why.empty(); })) return why;
    Wait(0.25);
    return PlayerNotReady();
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
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) {
                    const float d = Distance(p->Location, g_savedAt);
                    if (d >= 150.f) return "the player is " + std::to_string(static_cast<int>(d)) +
                                           " units from the saved position after loading it";
                    // The controller left in its default state ignores movement and jump input after a load. (The SDK's
                    // GetStateName() always answers None; IsInState works.)
                    return c->IsInState(game::Name("PlayerWalking"), false)
                               ? std::string()
                               : std::string("the controller is not in PlayerWalking after loading (no move or jump input)"); }); },
                {}};
    if (name == "Refill Reaction Time")
        return {[] { if (auto* c = game::Controller()) c->ReactionTimeEnergy = 0.f; },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(c->ReactionTimeEnergy >= 99.f, "the reaction time meter was not refilled"); }); },
                {}};
    if (name == "Clear Frame") {
        // The live engine (not its class default): one walk of the object list, it lives as long as the game
        static Classes::UEngine* engine = nullptr;
        static bool was = false;
        return {[] {
                    engine = nullptr;
                    const auto& objects = Classes::UObject::GetGlobalObjects();
                    for (std::size_t i = 0; i < objects.Num() && !engine; ++i) {
                        auto* o = objects.GetByIndex(i);
                        if (o && game::IsA(o, Classes::UGameEngine::StaticClass()) && o->GetName().rfind("Default__", 0) != 0)
                            engine = static_cast<Classes::UEngine*>(o);
                    }
                    if (engine) was = engine->bEnableColorClear != 0;
                },
                [] { return Expect(engine && engine->bEnableColorClear, "the engine's bEnableColorClear is not set"); },
                [] { return Expect(engine && (engine->bEnableColorClear != 0) == was,
                                   "bEnableColorClear not put back after switching off"); }};
    }
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
    else if (const std::string why = WaitPlayerReady(30.0); !why.empty()) {
        // Nothing that moves the player could be checked
        p.Problem("30 s after loading, the player can't be moved: " + why + " (pick another checkpoint)");
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

// The TAS module's "As fast as possible" for the length of a test (its playbacks then run unpaced), put back after.
class ScopedTasSpeed {
public:
    explicit ScopedTasSpeed(bool fast)
    {
        if (!fast) return;
        modules::ForEach([&](modules::Tab&, Module& m) {
            if (m.Name() == "TAS / Macro") box_ = dynamic_cast<ctl::Checkbox*>(m.FindSetting("As fast as possible"));
        });
        if (!box_) return;
        was_ = box_->Get();
        OnRenderThread([this] { box_->Set(true); });
        WaitTicks(2);
    }
    ~ScopedTasSpeed()
    {
        if (!box_) return;
        OnRenderThread([this] { box_->Set(was_); });
        WaitTicks(2);
    }
    ScopedTasSpeed(const ScopedTasSpeed&) = delete;
    ScopedTasSpeed& operator=(const ScopedTasSpeed&) = delete;

private:
    ctl::Checkbox* box_ = nullptr;
    bool was_ = false;
};

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
    if (const std::string why = WaitPlayerReady(10.0); !why.empty()) {
        p.Problem("the player can't be moved: " + why);
        p.Check(1, false);
        End(s);
        return;
    }

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
    bool airborne = false, started = false, pausedOnce = false;
    tas::Status st{};
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline && !g_aborted) {
        st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame >= st.frames)) break;   // Done (or paused at the end)
        if (started && !pausedOnce && !st.armed && st.frame >= 20) {
            // Paused, the game thread stands still and draws the menu itself, through every overlay's Present hook
            // (a crash on 2026-11-02). Not OnGameThread meanwhile: the game doesn't tick while paused.
            pausedOnce = true;
            gui::visible = true;
            const unsigned before = hooks::StalledFrames();
            const ULONGLONG pausedAt = GetTickCount64();
            tas::TogglePause();
            // 20 menu frames drawn by the paused game thread (or 3 s without them: the problem below)
            WaitFor(3.0, [before] { return hooks::StalledFrames() - before >= 20; });
            if (!tas::GetStatus().paused) p.Problem("pausing the playback didn't pause it");
            const unsigned drawn = hooks::StalledFrames() - before;
            if (drawn == 0) p.Problem("the menu was not drawn while paused");
            s.notes.push_back("paused " + std::to_string(GetTickCount64() - pausedAt) + " ms: " + std::to_string(drawn) +
                              " menu frames drawn meanwhile");
            tas::TogglePause();
            gui::visible = false;
            continue;
        }
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

// Camera keyframes: a macro with no input but two camera keys 90 degrees apart must turn the player by 90 degrees,
// the TAS setting the look direction itself. The timeline window is open meanwhile (drawn in the game).
void TestTasCamera()
{
    Step& s = Begin("TAS camera keys");
    Probe p(s);
    if (!tas::Available()) {
        p.Problem("the TAS engine hooks were not found");
        p.Check(1, false);
        End(s);
        return;
    }

    std::int32_t startYaw = 0;
    bool ok = false;
    OnGameThread([&] {
        if (auto* pc = game::Controller(); pc && game::Pawn()) {
            startYaw = static_cast<std::int32_t>(pc->Rotation.Yaw);
            ok = true;
            player_state::Save();
        }
    });
    if (!ok) {
        p.Problem("no player to turn");
        p.Check(1, false);
        End(s);
        return;
    }

    constexpr double kUnits = 65536.0 / 360.0;
    const float yaw0 = static_cast<float>(startYaw / kUnits);
    tas::Macro m(60.f);
    m.Describe("memenu self-test: turn 90 degrees with camera keys").Wait(40);
    tas::Replay macro = m.Build();
    tas::timeline::SetCameraKey(macro, {5, yaw0, 0.f, tas::Ease::Linear});
    tas::timeline::SetCameraKey(macro, {30, yaw0 + 90.f, 0.f, tas::Ease::Smooth});

    // The TAS module's timeline window, open while it plays
    ctl::Checkbox* timeline = nullptr;
    modules::ForEach([&](modules::Tab&, Module& mod) {
        for (ctl::Setting* c : mod.Settings())
            if (c->Name() == "Timeline" && mod.Name() == "TAS / Macro") timeline = dynamic_cast<ctl::Checkbox*>(c);
    });
    const bool timelineWas = timeline && timeline->Get();

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([timeline] {
        gui::visible = true;
        if (timeline) timeline->Set(true);
    });
    tas::Play();

    bool started = false;
    tas::Status st{};
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline && !g_aborted) {
        st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame >= st.frames)) break;
        Sleep(20);
    }
    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");

    std::int32_t endYaw = startYaw;
    OnGameThread([&] {
        if (auto* pc = game::Controller()) endYaw = static_cast<std::int32_t>(pc->Rotation.Yaw);
    });
    const double turned = (endYaw - startYaw) / kUnits;
    char info[96];
    std::snprintf(info, sizeof info, "turned %.2f degrees (keys: %.2f -> %.2f)", turned, yaw0, yaw0 + 90.f);
    if (!started) p.Problem("playback never started: " + tas::GetStatus().message);
    else if (std::abs(turned - 90.0) > 0.5) p.Problem(std::string("the camera keys didn't turn the player: ") + info);
    else s.notes.push_back(info);

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([timeline, timelineWas] {
        if (timeline) timeline->Set(timelineWas);
    });
    OnGameThread([] { player_state::Load(); });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// Go to + frame advance, the replay editor's way: from idle, go to frame 20 of a macro (it plays up to there and
// pauses), step one frame, and stop. The Replay Editor window is open meanwhile (drawn in the game).
void TestTasGoto()
{
    Step& s = Begin("TAS go to and step");
    Probe p(s);
    if (!tas::Available()) {
        p.Problem("the TAS engine hooks were not found");
        p.Check(1, false);
        End(s);
        return;
    }

    tas::Macro m(60.f);
    m.Describe("memenu self-test: go to and step").Wait(60);
    const tas::Replay macro = m.Build();

    ctl::Checkbox* editor = nullptr;
    modules::ForEach([&](modules::Tab&, Module& mod) {
        for (ctl::Setting* c : mod.Settings())
            if (c->Name() == "Replay editor" && mod.Name() == "TAS / Macro") editor = dynamic_cast<ctl::Checkbox*>(c);
    });
    const bool editorWas = editor && editor->Get();

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([editor] {
        gui::visible = true;
        if (editor) editor->Set(true);
    });

    // Not OnGameThread while paused: the game doesn't tick then
    const auto pausedOn = [](std::size_t frame) {
        const tas::Status st = tas::GetStatus();
        return st.mode == tas::Mode::Playing && st.paused && st.frame == frame;
    };
    tas::Goto(20);
    if (!WaitFor(10.0, [&] { return pausedOn(20); })) {
        const tas::Status st = tas::GetStatus();
        p.Problem("go to 20 didn't pause on frame 20 (frame " + std::to_string(st.frame) +
                  (st.paused ? ", paused" : ", running") + "): " + st.message);
    } else {
        tas::Advance(1);
        if (!WaitFor(5.0, [&] { return pausedOn(21); }))
            p.Problem("stepping from 20 didn't stop on 21 (frame " + std::to_string(tas::GetStatus().frame) + ")");
        else s.notes.push_back("went to frame 20, stepped to 21");
    }
    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([editor, editorWas] {
        if (editor) editor->Set(editorWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// The TAS's workflow helpers, with a "now" macro recorded on the spot:
//   1. a recording is autosaved when it stops, and counts as not saved until it is
//   2. bookmarks survive a save and a load (and count as changes)
//   3. "At the end: Loop" plays it again from the start
//   4. while a session runs, its keys don't reach module hotkeys (No Clip and TAS pause both on F5 by default)
// The user's macro, settings and files are put back.
void TestTasWorkflow()
{
    Step& s = Begin("TAS workflow");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::Available()) return fail("the TAS engine hooks were not found");
    Module* tasModule = nullptr;
    Module* noClip = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) {
        if (m.Name() == "TAS / Macro") tasModule = &m;
        if (m.Name() == "No Clip") noClip = &m;
    });
    auto* atEnd = tasModule ? dynamic_cast<ctl::Combo*>(tasModule->FindSetting("At the end")) : nullptr;
    auto* pauseKey = tasModule ? dynamic_cast<ctl::KeyHold*>(tasModule->FindSetting("Pause / resume")) : nullptr;
    if (!atEnd || !pauseKey) return fail("the TAS module's At the end / Pause key controls were not found");
    const int atEndWas = atEnd->Get();
    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; });

    namespace fs = std::filesystem;
    const auto newestAutosave = [] {
        fs::path newest;
        fs::file_time_type at{};
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(tas::AutosaveFolder(), ec))
            if (e.is_regular_file(ec) && e.last_write_time(ec) > at) { at = e.last_write_time(ec); newest = e.path(); }
        return newest;
    };
    const fs::path autosaveBefore = newestAutosave();
    const auto idle = [] { return tas::GetStatus().mode == tas::Mode::Idle; };

    // 1. Record a second, stop
    OnRenderThread([] { gui::visible = false; });
    tas::Record(tas::StartKind::Now, "", "", "");
    if (!WaitFor(10.0, [] { const auto st = tas::GetStatus(); return st.mode == tas::Mode::Recording && st.frame >= 60; }))
        p.Problem("the recording didn't get going");
    tas::Stop();
    WaitFor(5.0, idle);
    const fs::path autosave = newestAutosave();
    if (autosave.empty() || autosave == autosaveBefore) p.Problem("the recording was not autosaved");
    else s.notes.push_back("autosaved as " + autosave.filename().string());
    if (!tas::Unsaved()) p.Problem("a new recording doesn't count as not saved");

    // 2. Bookmarks through a save and a load
    const fs::path file = tas::Folder() / "memenu self-test.merp";
    tas::WithReplay([](tas::Replay& r) { r.SetMarker(10, "self-test mark"); });
    std::string error;
    if (!tas::SaveFile(file, &error)) p.Problem("saving failed: " + error);
    else if (tas::Unsaved()) p.Problem("still not saved after saving");
    tas::WithReplay([](tas::Replay& r) { r.EraseMarker(10); });
    if (!tas::Unsaved()) p.Problem("deleting a bookmark doesn't count as a change");
    if (!tas::LoadFile(file, &error)) p.Problem("loading failed: " + error);
    bool marked = false;
    WaitFor(5.0, [&] {
        tas::WithReplay([&](tas::Replay& r) { const auto* m = r.MarkerAt(10); marked = m && m->name == "self-test mark"; });
        return marked;
    });
    if (!marked) p.Problem("the bookmark was not in the saved file");
    else if (tas::Unsaved()) p.Problem("a macro just loaded counts as not saved");
    std::error_code ec;
    fs::remove(file, ec);

    // Undo through the editors' shared history, on the replay in the game
    bool undone = false;
    tas::WithReplay([&](tas::Replay& r) {
        tasui::History().Watch(r);
        tasui::History().Push(r, "self-test bookmark");
        r.SetMarker(20, "undo me");
        tasui::History().Watch(r);
        undone = tasui::History().Undo(r) && !r.MarkerAt(20) && r.MarkerAt(10);
    });
    if (!undone) p.Problem("undo didn't take the edit back");

    // 3. Loop, and 4. the session's keys while it plays. The timeline and the replay editor are open meanwhile (drawn
    // with a macro running: they must not trip over it).
    auto* timelineBox = dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Timeline"));
    auto* editorBox = dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Replay editor"));
    const bool timelineWas = timelineBox && timelineBox->Get(), editorWas = editorBox && editorBox->Get();
    OnRenderThread([&] {
        atEnd->Set(static_cast<int>(tas::AtEnd::Loop));
        gui::visible = true;
        if (timelineBox) timelineBox->Set(true);
        if (editorBox) editorBox->Set(true);
    });
    WaitTicks(2);
    tas::Play();
    bool nearEnd = false, looped = false;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline && !g_aborted && !looped) {
        const auto st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing && !st.armed && st.frames > 0) {
            if (st.frame + 5 >= st.frames) nearEnd = true;
            else if (nearEnd && st.frame < 20) looped = true;
        }
        Sleep(5);
    }
    if (!looped) p.Problem(std::string("Loop didn't play it again (") + (nearEnd ? "reached the end" : "never reached the end") + ")");
    else s.notes.push_back("looped back to the start");

    if (noClip && tas::GetStatus().mode == tas::Mode::Playing) {
        const int vk = pauseKey->Get(), keyWas = noClip->key;
        const bool onWas = noClip->Enabled();
        bool switched = false;
        OnRenderThread([&] {
            noClip->key = vk;               // The same key as the TAS's pause, like the defaults
            modules::OnKey(vk);
            switched = noClip->Enabled() != onWas;
            if (switched) noClip->SetEnabled(onWas);
            noClip->key = keyWas;
        });
        if (switched) p.Problem("the TAS's pause key also switched No Clip during a session");
        else s.notes.push_back("the pause key didn't reach No Clip during the session");
    }
    tas::Stop();
    WaitFor(5.0, idle);

    // Back as it was
    if (!autosave.empty() && autosave != autosaveBefore) fs::remove(autosave, ec);
    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        atEnd->Set(atEndWas);
        if (timelineBox) timelineBox->Set(timelineWas);
        if (editorBox) editorBox->Set(editorWas);
        gui::visible = true;
    });
    WaitTicks(3);
    p.Check(1, true);
    End(s);
}

#if MEMENU_FEATURE_INPUT_OVERLAY
// The input overlay during a TAS playback: a macro holds the forward key for half a second; the input the engine gets
// (what the overlay draws) must show it held while it plays and let go after. The overlay writes its layouts on first use.
void TestInputOverlay()
{
    Step& s = Begin("Input overlay");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::input::Installed()) return fail("the engine's input monitor isn't installed");
    Module* overlay = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Input Overlay") overlay = &m; });
    if (!overlay) return fail("the Input Overlay module was not found");
    const bool enabledWas = overlay->Enabled();
    OnRenderThread([&] { overlay->SetEnabled(true); });

    std::string forward;
    OnGameThread([&] { forward = KeyFor(game::Controller(), "GBA_MoveForward", "W"); });
    std::string key = forward;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    tas::Macro m(60.f);
    m.Describe("memenu self-test: input overlay").Wait(5).Press(forward).Wait(30).Release(forward).Wait(15);
    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = m.Build(); });
    OnRenderThread([] { gui::visible = false; });
    const auto pressesOf = [&key](const tas::input::Snapshot& sn) {
        const auto it = sn.keys.find(key);
        return it == sn.keys.end() ? 0u : it->second.presses;
    };
    const unsigned pressesBefore = pressesOf(tas::input::Get());
    tas::Play();

    bool started = false, sawHeld = false;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline && !g_aborted) {
        const auto st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame + 1 >= st.frames)) break;
        const auto snap = tas::input::Get();
        if (const auto it = snap.keys.find(key); it != snap.keys.end() && it->second.held) sawHeld = true;
        Sleep(2);
    }
    tas::Stop();
    WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
    WaitTicks(5);
    const auto after = tas::input::Get();
    const auto it = after.keys.find(key);
    const unsigned pressed = pressesOf(after) - pressesBefore;
    if (!started) p.Problem("the macro didn't play");
    else if (pressed != 1) p.Problem("the overlay counted " + std::to_string(pressed) + " presses of " + forward + ", not 1");
    else if (it != after.keys.end() && it->second.held) p.Problem(forward + " still shows as held after the playback");
    else s.notes.push_back(forward + " pressed once during the playback" + (sawHeld ? " (seen held)" : "") +
                           ", let go after");

    std::error_code ec;
    if (!std::filesystem::exists(paths::Dir() / "input_layouts" / "Movement.json", ec))
        p.Problem("the layout files were not written (input_layouts\\Movement.json)");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        overlay->SetEnabled(enabledWas);
        gui::visible = true;
    });
    WaitTicks(3);
    p.Check(1, true);
    End(s);
}
#endif

// Desync prevention and fixing, with a macro that starts at the self-test's checkpoint (only those are checked):
//   1. the first playback records the reference player states, with the macro's recorded mouse sensitivity set for it
//      (and the player's own back afterwards)
//   2. the recorded positions are moved 10 units from frame 30 on: with "On desync: Pause" it stops on frame 30 and
//      says the position is off by 10
//   3. with "Correct small drift" it plays to the end, putting the player back on the recording
void TestTasDesync()
{
    Step& s = Begin("TAS desync fixing");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::Available()) return fail("the TAS engine hooks were not found");

    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) { index = i; break; }
    if (index < 0) return fail("no level matching '" + g_options.map + "'");
    const int cp = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));

    Module* tasModule = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "TAS / Macro") tasModule = &m; });
    auto* onDesync = tasModule ? dynamic_cast<ctl::Combo*>(tasModule->FindSetting("On desync")) : nullptr;
    auto* match = tasModule ? dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Match the recording")) : nullptr;
    if (!onDesync || !match) return fail("the TAS module's On desync / Match the recording controls were not found");
    const int onDesyncWas = onDesync->Get();
    const bool matchWas = match->Get();

    std::string forward;
    float sens = 0.f;
    OnGameThread([&] {
        auto* pc = game::Controller();
        forward = KeyFor(pc, "GBA_MoveForward", "W");
        if (pc && pc->PlayerInput) sens = pc->PlayerInput->MouseSensitivity;
    });
    const float recordedSens = sens + 1.f;

    tas::Macro m(60.f);
    m.Describe("memenu self-test: desync fixing").Wait(5).Press(forward).Wait(60).Release(forward).Wait(10);
    tas::Replay macro = m.Build();
    macro.start = tas::StartKind::Checkpoint;
    macro.map = maps[index].label;
    macro.checkpoint = cp > 0 ? maps[index].checkpoints[static_cast<std::size_t>(cp - 1)] : std::string();
    macro.syncValidUntil = 0;                       // the first playback records the reference
    char text[32];
    std::snprintf(text, sizeof text, "%.4f", recordedSens);
    macro.extensions["env.sensitivity"] = text;
    macro.extensions["env.invertMouse"] = "0";
    macro.extensions["env.blockStreaming"] = "1";

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([&] {
        gui::visible = false;
        onDesync->Set(static_cast<int>(tas::OnDesync::Pause));
        match->Set(true);
    });
    WaitTicks(3);

    // Plays it (loading the checkpoint), until it ends or pauses; the status then
    float sensDuring = 0.f;
    const auto play = [&]() {
        tas::Play();
        bool started = false, sampled = false;
        tas::Status st{};
        const ULONGLONG deadline = GetTickCount64() + 90000;
        while (GetTickCount64() < deadline && !g_aborted) {
            st = tas::GetStatus();
            if (st.mode == tas::Mode::Playing && !st.armed) started = true;
            if (started && !sampled && st.frame > 10 && !st.paused) {
                OnGameThread([&] {
                    if (auto* pc = game::Controller(); pc && pc->PlayerInput) sensDuring = pc->PlayerInput->MouseSensitivity;
                }, 1.0);
                sampled = true;
            }
            if (started && (st.mode != tas::Mode::Playing || st.paused || st.frame >= st.frames)) break;
            Sleep(20);
        }
        tas::Stop();
        WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
        return std::make_pair(started, st);
    };

    // 1. The reference
    const auto [ran, first] = play();
    std::size_t recorded = 0;
    tas::WithReplay([&](tas::Replay& r) { recorded = r.syncValidUntil; });
    float sensAfter = 0.f;
    OnGameThread([&] {
        if (auto* pc = game::Controller(); pc && pc->PlayerInput) sensAfter = pc->PlayerInput->MouseSensitivity;
    });
    if (!ran) p.Problem("the first playback never started: " + tas::GetStatus().message);
    else if (first.desyncFrame >= 0) p.Problem("the first playback desynced, with nothing to compare against yet");
    else if (recorded < macro.FrameCount()) p.Problem("the first playback recorded " + std::to_string(recorded) + " of " +
                                                      std::to_string(macro.FrameCount()) + " reference states");
    if (std::abs(sensDuring - recordedSens) > 1e-3f)
        p.Problem("playback didn't use the recorded mouse sensitivity (" + std::to_string(sensDuring) + ", recorded " +
                  std::to_string(recordedSens) + ")");
    if (std::abs(sensAfter - sens) > 1e-3f)
        p.Problem("the player's own mouse sensitivity wasn't restored (" + std::to_string(sensAfter) + ", was " +
                  std::to_string(sens) + ")");

    // 2. Off by 10 from frame 30: Pause stops there and says so
    tas::WithReplay([&](tas::Replay& r) {
        for (std::size_t f = 30; f < r.sync.size(); ++f) r.sync[f].x += 10.f;
    });
    const auto [ran2, paused] = play();
    if (!ran2) p.Problem("the second playback never started");
    else if (paused.desyncFrame != 30)
        p.Problem("with the recording moved on frame 30, the desync was found on " + std::to_string(paused.desyncFrame));
    else if (paused.desyncInfo.find("position off by 10") == std::string::npos)
        p.Problem("the desync was described as '" + paused.desyncInfo + "'");
    else s.notes.push_back("paused at frame 30: " + paused.desyncInfo);

    // 3. Correct puts the player back and plays to the end
    OnRenderThread([&] { onDesync->Set(static_cast<int>(tas::OnDesync::Correct)); });
    WaitTicks(3);
    const auto [ran3, corrected] = play();
    if (!ran3) p.Problem("the third playback never started");
    else if (corrected.desyncFrame >= 0)
        p.Problem("Correct didn't fix the desync on frame " + std::to_string(corrected.desyncFrame) + ": " +
                  corrected.desyncInfo);
    else if (corrected.corrections == 0) p.Problem("Correct made no corrections");
    else if (corrected.frame < macro.FrameCount()) p.Problem("Correct stopped on frame " + std::to_string(corrected.frame));
    else s.notes.push_back("corrected " + std::to_string(corrected.corrections) + " frames, played to the end");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        onDesync->Set(onDesyncWas);
        match->Set(matchWas);
        gui::visible = true;
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}
#endif

#if MEMENU_FEATURE_COLLISION_VIEW
// The collision view's inspector: with the panel open it lists the volumes near the player; facing one of them (the
// Nearby list's click) must make the look-at report that same volume: as the one looked at, or on the line behind
// another volume that hides it (the "behind" list). Volumes the player is inside can't be checked that way: up to five
// are tried, and one has to match.
void TestCollisionInspect()
{
    Step& s = Begin("Collision view inspect");
    Probe p(s);
    Module* view = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Collision View") view = &m; });
    auto* inspect = view ? dynamic_cast<ctl::Section*>(view->FindSetting("Inspect")) : nullptr;
    if (!view || !inspect) {
        p.Problem("the Collision View module or its Inspect section was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = view->Enabled(), expandedWas = view->expanded, inspectWas = inspect->Get();
    unsigned yawWas = 0, pitchWas = 0;
    OnGameThread([&] { if (auto* pc = game::Controller()) { yawWas = pc->Rotation.Yaw; pitchWas = pc->Rotation.Pitch; } });
    OnRenderThread([&] {
        view->expanded = true;
        inspect->Set(true);
        if (!view->Enabled()) view->SetEnabled(true);
    });

    std::vector<debugworld::Info::Near> nearby;
    WaitFor(10.0, [&] { nearby = debugworld::GetInfo().nearby; return !nearby.empty(); });
    if (nearby.empty()) p.Problem("no volumes listed near the player");
    int tried = 0, matched = 0;
    for (const auto& n : nearby) {
        if (tried >= 5 || g_aborted) break;
        if (n.distance <= 0.f || n.name.empty()) continue;      // Inside it: the view ray starts in it
        ++tried;
        debugworld::FaceTowards(n.x, n.y, n.z);
        WaitTicks(15);
        const auto look = debugworld::GetInfo().look;
        const bool front = look.valid && look.name == n.name;
        const bool behindIt = look.valid && std::any_of(look.behind.begin(), look.behind.end(),
                                                        [&](const auto& b) { return b.first == n.name; });
        const bool ok = front || behindIt;
        matched += ok ? 1 : 0;
        const std::string what = front      ? "looked at it"
                                 : behindIt ? "on the line behind " + look.name
                                 : look.valid ? "looked at " + look.name : "looked at nothing";
        char note[200];
        std::snprintf(note, sizeof note, "faced %s (center %.0f away): %s, hit %.0f away", n.name.c_str(), n.distance,
                      what.c_str(), look.distance);
        s.notes.push_back(note);
        std::string behind;
        for (const auto& [name, at] : look.behind) behind += " " + name + "@" + std::to_string(static_cast<int>(at));
        if (!behind.empty()) s.notes.push_back("  also on the line:" + behind);
        if (ok) break;
    }
    s.notes.push_back(std::to_string(nearby.size()) + " volumes listed nearby");
    if (!nearby.empty() && tried == 0) p.Problem("the player is inside every volume listed nearby: nothing to face");
    if (tried > 0 && matched == 0) p.Problem("facing a nearby volume never made the look-at report it");

    OnGameThread([&] { if (auto* pc = game::Controller()) { pc->Rotation.Yaw = yawWas; pc->Rotation.Pitch = pitchWas; } });
    OnRenderThread([&] {
        inspect->Set(inspectWas);
        view->expanded = expandedWas;
        view->SetEnabled(enabledWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}
#endif

#if MEMENU_FEATURE_SPEEDRUN_TIMER
// The speedrun timer against the game:
//   1. ProcessEvent is hooked and the game's level completed event (the auto split) was found
//   2. reloading the level starts an individual level run (auto start)
//   3. a second reload while it runs (auto reset off) is not counted: real time grows, load removed time doesn't
//   4. a manual split finishes it (one segment) and sets a personal best; undo takes it back
// The level's record file is put back the way it was.
void TestSpeedrunTimer()
{
    Step& s = Begin("Speedrun timer");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };

    Module* timer = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Speedrun Timer") timer = &m; });
    auto* category = timer ? dynamic_cast<ctl::Combo*>(timer->FindSetting("Category")) : nullptr;
    auto* autoStart = timer ? dynamic_cast<ctl::Checkbox*>(timer->FindSetting("Auto start")) : nullptr;
    auto* autoReset = timer ? dynamic_cast<ctl::Checkbox*>(timer->FindSetting("Auto reset")) : nullptr;
    if (!category || !autoStart || !autoReset) return fail("the Speedrun Timer module or its controls were not found");

    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) { index = i; break; }
    if (index < 0) return fail("no level matching '" + g_options.map + "'");
    const int cp = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));

    const bool enabledWas = timer->Enabled();
    const int categoryWas = category->Get();
    const bool autoStartWas = autoStart->Get(), autoResetWas = autoReset->Get();
    OnRenderThread([&] {
        category->Set(static_cast<int>(speedrun::Category::IndividualLevel));
        autoStart->Set(true);
        autoReset->Set(true);
        if (timer->Enabled()) timer->SetEnabled(false);     // A clean start: no run left from before
        timer->SetEnabled(true);
    });
    WaitTicks(10);
    speedrun::Reset();
    WaitTicks(5);

    if (!WaitFor(10.0, [] { return game::ScriptEventsReady(); }))
        p.Problem("the level completed event is not watched (no auto splits): see the log for ProcessEvent");
    else s.notes.push_back("ProcessEvent hooked, level completed event found");

    // Its record file, kept to put back
    const std::string map = speedrun::Snapshot().map;
    const std::string title = speedrun::ChapterTitle(map);
    const auto file = paths::Dir() / "speedrun" / (speedrun::FileName("IL - " + (title.empty() ? map : title)) + ".json");
    std::string fileWas;
    const bool hadFile = std::filesystem::exists(file);
    if (hadFile) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        fileWas = ss.str();
    }

    const auto reload = [&]() -> bool {
        const int before = g_loadsFinished.load();
        OnRenderThread([&] { levels::Load(index, cp, levels::Method::Game, false); });
        if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > before; })) { p.Problem("the level did not reload"); return false; }
        if (!WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); })) { p.Problem("no player after reloading"); return false; }
        WaitPlayerReady(30.0);
        return true;
    };

    if (reload()) {
        WaitFor(5.0, [] { const auto sv = speedrun::Snapshot(); return sv.timer.Running() && sv.timer.Lrt(sv.now) > 0.1; });
        auto v = speedrun::Snapshot();
        if (!v.timer.Running()) p.Problem("no run started when the level loaded (map '" + v.map + "', last: " + v.lastEvent + ")");
        else {
            s.notes.push_back("started: " + v.lastEvent);
            OnRenderThread([&] { autoReset->Set(false); });
            WaitTicks(3);
            v = speedrun::Snapshot();
            const double lrt0 = v.timer.Lrt(v.now), rta0 = v.timer.Rta(v.now);
            if (reload()) {
                v = speedrun::Snapshot();
                const double lrt = v.timer.Lrt(v.now) - lrt0, rta = v.timer.Rta(v.now) - rta0;
                char buf[160];
                std::snprintf(buf, sizeof buf, "across a reload: real time +%.2f s, load removed time +%.2f s", rta, lrt);
                s.notes.push_back(buf);
                if (!v.timer.Running()) p.Problem("the run stopped on a reload with auto reset off");
                else if (rta - lrt < 0.3) p.Problem(std::string("the load was not removed: ") + buf);
                else if (lrt <= 0.0) p.Problem(std::string("the load removed time did not move: ") + buf);

                const int completedBefore = v.comparison.completed;
                speedrun::StartOrSplit();               // The only segment: finishes the run
                WaitTicks(5);
                v = speedrun::Snapshot();
                if (v.timer.GetPhase() != speedrun::Timer::Phase::Finished) p.Problem("a split on the only segment did not finish the run");
                else if (v.comparison.completed != completedBefore + 1) p.Problem("the finished run was not counted");
                else if (v.comparison.PbTime() < 0.0) p.Problem("the finished run left no personal best");
                else s.notes.push_back("finished in " + speedrun::Format(v.timer.FinalLrt(), 3) + (v.newPb ? ", a personal best" : ""));

                speedrun::Undo();
                WaitTicks(5);
                v = speedrun::Snapshot();
                if (!v.timer.Running()) p.Problem("undoing the final split did not resume the run");
                else if (v.comparison.completed != completedBefore) p.Problem("undoing the final split did not take back the finish");
            }
        }
    }

    // Back as it was: a different category first, so the restored file is read again when the old one comes back
    speedrun::Reset();
    WaitTicks(3);
    OnRenderThread([&] { category->Set(static_cast<int>(speedrun::Category::AnyPercent)); });
    WaitTicks(5);
    std::error_code ec;
    if (hadFile) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << fileWas;
    } else std::filesystem::remove(file, ec);
    OnRenderThread([&] {
        category->Set(categoryWas);
        autoStart->Set(autoStartWas);
        autoReset->Set(autoResetWas);
        timer->SetEnabled(enabledWas);
    });
    WaitTicks(5);
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

// Modules that draw on the screen every frame while on (a HUD, a counter): with the menu closed, the frame must have more
// in it while they are on than before, and no more after they are switched off.
const std::set<std::string> kDrawsHud = {"Crosshair", "Show FPS", "Watermark", "Speedometer", "Input Overlay",
                                         "Speedrun Timer"};

// What the menu draws with its window closed (the modules' HUDs), a few frames after a change.
unsigned HudVertices()
{
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(3);
    return hooks::MenuVertices();
}

void TestModule(Module& m)
{
    Step& s = Begin("module " + m.Tab() + "/" + m.Name());
    const unsigned n = g_options.ticks;
    Probe p(s);
    const bool drawsHud = kDrawsHud.count(m.Name()) != 0;
    unsigned hudBefore = 0;
    if (drawsHud) {
        OnRenderThread([&m] { if (m.Enabled()) m.SetEnabled(false); });
        hudBefore = HudVertices();
        OnRenderThread([] { gui::visible = true; });
    }
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
    WaitTicks(std::max(3u, n / 2));
    if (drawsHud) {
        const unsigned hudOn = hooks::MenuVertices();
        if (hudOn <= hudBefore)
            p.Problem("draws nothing with the menu closed (" + std::to_string(hudOn) + " vertices, " +
                      std::to_string(hudBefore) + " before it was on)");
        else s.notes.push_back("draws " + std::to_string(hudOn - hudBefore) + " vertices with the menu closed");
    }
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
    if (drawsHud) {
        const unsigned hudOff = HudVertices();
        if (hudOff > hudBefore)
            p.Problem("still draws after switching off (" + std::to_string(hudOff) + " vertices, " +
                      std::to_string(hudBefore) + " before it was on)");
        OnRenderThread([] { gui::visible = true; });
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
        else if (key == "fast") g_options.fast = value != "0";
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

// The engine's frame rate cap (UGameEngine.bSmoothFrameRate, 62 fps): off while testing, so frame and tick counted waits
// go as fast as the PC draws. Put back as it was.
void SetFrameRateCap(bool on)
{
#if MEMENU_GAME
    static bool was = true, changed = false;
    if (on && !changed) return;
    OnGameThread([&] {
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num(); ++i) {
            auto* o = objects.GetByIndex(i);
            if (!o || !game::IsA(o, Classes::UGameEngine::StaticClass()) || o->GetName().rfind("Default__", 0) == 0) continue;
            auto* engine = static_cast<Classes::UGameEngine*>(o);
            if (on) engine->bSmoothFrameRate = was ? 1 : 0;
            else {
                was = engine->bSmoothFrameRate != 0;
                engine->bSmoothFrameRate = 0;
                changed = true;
            }
            spdlog::info("Self-test: frame rate cap {}", on ? "back on" : "off (fast mode)");
            break;
        }
    });
    if (on) changed = false;
#else
    (void)on;
#endif
}

void Run()
{
    if (!g_requested) return;
#if MEMENU_GAME
    game::OnLevelLoad(&OnLevelLoad);
#endif
    const auto saved = SaveAll();
    notify::SetMuted(true);         // Nothing on screen but what the modules draw (the HUD checks measure it)
    if (g_options.fast) SetFrameRateCap(false);

    if (TestFrames()) {
#if MEMENU_GAME
        if (TestGame() && !g_aborted && TestLevel() && !g_aborted && Lower(g_options.map) != "none") {
            TestInputBlocking();
            // The feature tests, by name (--only / --skip pick them like modules). `fastTas`: its playbacks don't
            // depend on pacing, so in fast mode they run as fast as they go.
            struct Named { const char* name; void (*run)(); bool fastTas; };
            const Named tests[] = {
#if MEMENU_FEATURE_TAS
                {"TAS macro", &TestTasMacro, true},
                {"TAS camera keys", &TestTasCamera, true},
                {"TAS go to and step", &TestTasGoto, true},
                {"TAS desync fixing", &TestTasDesync, true},
                {"TAS workflow", &TestTasWorkflow, false},      // Records and loops in real time
#if MEMENU_FEATURE_INPUT_OVERLAY
                {"Input overlay", &TestInputOverlay, true},
#endif
#endif
#if MEMENU_FEATURE_COLLISION_VIEW
                {"Collision view inspect", &TestCollisionInspect, false},
#endif
#if MEMENU_FEATURE_SPEEDRUN_TIMER
                {"Speedrun timer", &TestSpeedrunTimer, false},
#endif
                {nullptr, nullptr, false},
            };
            for (const Named* t = tests; t->name && !g_aborted; ++t) {
                if (!g_options.only.empty() && !g_options.only.count(t->name)) continue;
                if (g_options.skip.count(t->name)) continue;
#if MEMENU_FEATURE_TAS
                const ScopedTasSpeed speed(g_options.fast && t->fastTas);
#endif
                t->run();
            }
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
    if (g_options.fast) SetFrameRateCap(true);
    notify::SetMuted(false);
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
