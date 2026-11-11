#include "selftest.h"

#include "config.h"
#include "core/devmode.h"
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
#include "game/save_library.h"
#include "runtime/call.h"
#include "runtime/engine_calls.h"
#include "runtime/layout.h"
#include "runtime/lua_vm.h"
#include "runtime/mods.h"
#include "runtime/packages.h"
#if MEMENU_FEATURE_SPEEDRUN_TIMER
#include "speedrun/live.h"
#endif
#if MEMENU_FEATURE_COLLISION_VIEW
#include "game/debug_world.h"
#include "game/static_world.h"
#endif
#if MEMENU_FEATURE_INFO_LINES
#include "features/visual/info_lines.h"
#endif
#if MEMENU_FEATURE_KICKGLITCH_HELPER
#include "features/trainer/kickglitch_helper.h"
#endif
#if MEMENU_FEATURE_AUTO_KICKGLITCH
#include "features/trainer/auto_kickglitch.h"
#endif
#if MEMENU_FEATURE_KEY_BINDINGS
#include "features/misc/key_bindings.h"
#include <shlobj.h>
#endif
#if MEMENU_FEATURE_TAS
#include "features/tas/timeline_view.h"
#include "tas/input_monitor.h"
#include "tas/macro.h"
#include "tas/engine.h"
#include "tas/memory_map.h"
#include "tas/snapshot.h"
#include "tas/timeline.h"
#include "tas/tas.h"
#endif

#include "sdk/SDK.hpp"
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
#include <unordered_map>
#include <string>
#include <thread>
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
    bool keepLevel = false;     // Run from the menu: test in the level the player is in (no level load)
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
bool g_aborted = false;         // Unload requested (or Stop) in the middle: stop testing

// Runs from the menu (developer mode, Start): on a thread of their own, published for the Test module's window
std::atomic<bool> g_stop{false};        // Stop(): the waits give up, the run ends and puts everything back
std::atomic<bool> g_restoring{false};   // Putting settings back: waits work even after Stop or an unload request
std::atomic<bool> g_interactive{false}; // A run from the menu is going
std::thread g_worker;
std::mutex g_publishMutex;
Progress g_published;
int g_runs = 0;

// Buttons that would open windows on the desktop, delete files or run for minutes: never pressed
const std::set<std::string> kNoPress = {"Open log file", "Open themes folder", "Open save states folder",
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

// Copies the steps for the Test module's window (any thread reads them through Snapshot)
void Publish()
{
    std::lock_guard lock(g_publishMutex);
    g_published.seconds = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
    g_published.steps.clear();
    for (const Step& st : g_steps) {
        StepView v;
        v.name = st.name;
        v.seconds = st.seconds;
        v.problems = st.problems;
        v.notes = st.notes;
        const bool current = &st == &g_steps.back() && st.name == g_current;
        v.state = !st.ok ? StepView::State::Failed : current ? StepView::State::Running : StepView::State::Passed;
        g_published.steps.push_back(std::move(v));
    }
}

void WriteResult(bool finished)
{
    Publish();
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
// Unload requested or Stop() pressed: the test ends. Not while settings are put back afterwards (that must finish).
bool Interrupted()
{
    return !g_restoring && (g_stop || unload::Requested());
}

// Sleeps in small steps; false if unload was requested meanwhile (the test stops then).
bool Wait(double seconds)
{
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (GetTickCount64() < end) {
        if (Interrupted()) { g_aborted = true; return false; }
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
        if (Interrupted()) { g_aborted = true; return false; }
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
#if MEMENU_FEATURE_TAS
    // Where the game's threads are (once per test run: enough to see a hang)
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        for (const auto& t : tas::memmap::Threads()) {
            if (t.startModule != "mirrorsedge.exe" && t.startModule.rfind("physx", 0) != 0) continue;
            std::string line;
            for (const auto& w : tas::memmap::StackSample(t.id, 28)) line += w + " ";
            spdlog::warn("Stall: thread {} ({}) state {} wait {}: {}", t.id, t.startModule, t.state, t.waitReason, line);
        }
    }
#endif
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
#if MEMENU_FEATURE_INFO_LINES
    if (name == "Info Lines") {
        // The built-in example (a profile without a file), never the player's own: it must parse and evaluate without a
        // problem, draw lines with a player in the level, and its values must be the game's (x through fmt).
        static std::string savedProfile;
        static bool savedExpanded = false;
        static unsigned long long editorFramesBefore = 0;
        static constexpr const char* kTestProfile = "selftest (built-in example)";
        // The setting is read on the render thread: changed there too.
        return {[&m] {
                    editorFramesBefore = info_lines::EditorFrames();
                    hooks::RunOnRenderThread([&m] {
                        if (auto* t = dynamic_cast<ctl::Text*>(m.FindSetting("Profile"))) {
                            savedProfile = t->Get();
                            t->Set(kTestProfile);
                        }
                        savedExpanded = m.expanded;
                        m.expanded = true;                      // Its editor is drawn while the menu is open
                        info_lines::SetEditorOpenForTest(true);
                    });
                },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) -> std::string {
                    const info_lines::Status st = info_lines::LastStatus();
                    if (st.frames == 0) return "it never drew";
                    if (st.profile != kTestProfile) return "it shows profile '" + st.profile + "', not the test's";
                    if (!st.errors.empty())
                        return "the example has problems: line " + std::to_string(st.errors[0].line) + ": " + st.errors[0].message;
                    if (st.lines == 0) return "the example drew no lines with a player in the level";
                    std::vector<hud::Error> errors;
                    const std::string text = info_lines::Evaluate("{x:.0f}|{statename}|{map}", &errors);
                    if (!errors.empty()) return "evaluating values: " + errors[0].message;
                    const std::size_t bar = text.find('|');
                    const double x = std::atof(text.substr(0, bar).c_str());
                    if (std::fabs(x - p->Location.X) > 100.0)
                        return "{x} is " + text.substr(0, bar) + ", the player is at X " + std::to_string(p->Location.X);
                    if (text.find("|?") != std::string::npos || text.back() == '|')
                        return "a value is missing: '" + text + "'";
                    return std::string(); }); },
                [&m] {
                    hooks::RunOnRenderThread([&m] {
                        if (auto* t = dynamic_cast<ctl::Text*>(m.FindSetting("Profile"))) t->Set(savedProfile);
                        m.expanded = savedExpanded;
                        info_lines::SetEditorOpenForTest(false);
                    });
                    if (info_lines::EditorFrames() == editorFramesBefore) return std::string("its editor was never drawn");
                    return std::string();
                }};
    }
#endif
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
                    // GetStateName() always answers None; game::IsInState works.)
                    if (game::IsInState(c, "Dead")) return std::string("the controller reads as Dead: the state check is broken");
                    return game::IsInState(c, "PlayerWalking")
                               ? std::string()
                               : std::string("the controller is not in PlayerWalking after loading (no move or jump input)"); }); },
                {}};
    if (name == "Refill Reaction Time")
        return {[] { if (auto* c = game::Controller()) c->ReactionTimeEnergy = 0.f; },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(c->ReactionTimeEnergy >= 99.f, "the reaction time meter was not refilled"); }); },
                {}};
    if (name == "Save States") {
        // In a folder of its own, never the player's states. The check reads the file back from disk and loads it
        // after moving away: the stored format must bring the player back, in walking state.
        static std::filesystem::path folder;
        return {[] {
                    folder = paths::Dir() / "selftest-savestates";
                    std::error_code ec;
                    std::filesystem::remove_all(folder, ec);
                    save_library::SetFolder(folder);
                    if (auto* p = game::Pawn()) g_savedAt = p->Location;
                },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) -> std::string {
                    if (save_library::List(game::MapName()).size() != 1) return "running it did not add one state";
                    if (!std::filesystem::exists(folder / (save_library::Map() + ".json"))) return "no file was written";
                    save_library::SetFolder(folder);    // Forget it: read back from the file
                    p->Location.X += 2000.f;
                    p->Velocity = Classes::FVector{};
                    std::string error;
                    if (!save_library::Load(0, &error)) return "loading it back failed: " + error;
                    const float d = Distance(p->Location, g_savedAt);
                    if (d >= 150.f) return "the player is " + std::to_string(static_cast<int>(d)) +
                                           " units from the stored position after loading it";
                    return Expect(game::IsInState(c, "PlayerWalking"),
                                  "the controller is not in PlayerWalking after loading");
                }); },
                [] {
                    save_library::SetFolder({});
                    std::error_code ec;
                    std::filesystem::remove_all(folder, ec);
                    return std::string();
                }};
    }
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
    if (name == "Teleport to Spawn") {
        // Moved away and turned a quarter: the teleport must bring both back (the turn is SetRotation, a native-index
        // function the SDK's call never ran)
        static int turnedYaw = 0;
        return {[] {
                    if (auto* p = game::Pawn()) { p->Location.Z += 600.f; p->Location.X += 300.f; g_savedAt = p->Location; }
                    if (auto* c = game::Controller()) { c->Rotation.Yaw += 16384; turnedYaw = static_cast<int>(c->Rotation.Yaw); }
                },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) {
                    char where[160];
                    std::snprintf(where, sizeof where, "the player was not moved: at (%.0f, %.0f, %.0f), moved away to (%.0f, %.0f, %.0f)",
                                  p->Location.X, p->Location.Y, p->Location.Z, g_savedAt.X, g_savedAt.Y, g_savedAt.Z);
                    if (Distance(p->Location, g_savedAt) <= 200.f) return std::string(where);
                    return Expect(static_cast<int>(c->Rotation.Yaw) != turnedYaw, "the view was not turned to the spawn point (SetRotation)"); }); },
                {}};
    }
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
        Publish();
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
    if (g_options.keepLevel) {
        Step& s = Begin("level (current)");
        Probe p(s);
        if (!game::GetStatus().hasPawn || game::IsLoading())
            p.Problem("no player here: load a level first, or let the test load one");
        else if (const std::string why = WaitPlayerReady(10.0); !why.empty())
            p.Problem("the player can't be moved here: " + why);
        p.Check(1, true);
        End(s);
        return s.ok;
    }
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

// Opt-in investigation: the engine's rendering thread switched off (ToggleRenderingThread), then the level loaded again.
// If the load hangs, the game thread's stack is logged.
void TestRenderingThreadOff()
{
    Step& s = Begin("Rendering thread off");
    Probe p(s);
    const auto exeThreads = [] {
        int n = 0;
        for (const auto& t : tas::memmap::Threads()) n += t.startModule == "mirrorsedge.exe" ? 1 : 0;
        return n;
    };
    const int before = exeThreads();
    OnGameThread([] { game::ConsoleCommand("ToggleRenderingThread"); });
    WaitTicks(5);
    const int after = exeThreads();
    s.notes.push_back("exe threads " + std::to_string(before) + " -> " + std::to_string(after));

    std::atomic<bool> done{false};
    DWORD gameThread = 0;
    OnGameThread([&] { gameThread = GetCurrentThreadId(); });
    std::thread watchdog([&] {
        for (int i = 0; i < 80 && !done; ++i) Sleep(100);
        if (done) return;
        for (int round = 0; round < 3; ++round) {
            for (const auto& t : tas::memmap::Threads()) {
                if (t.startModule != "mirrorsedge.exe" && t.id != gameThread) continue;
                std::string line;
                for (const auto& w : tas::memmap::StackSample(t.id, 28)) line += w + " ";
                spdlog::warn("Rendering off: thread {}{} state {} wait {}: {}", t.id, t.id == gameThread ? " (game)" : "",
                             t.state, t.waitReason, line);
            }
            Sleep(1000);
        }
    });
    const int loads = g_loadsFinished.load();
    const auto maps = levels::Maps();
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) {
            OnGameThread([&] { levels::Load(i, g_options.checkpoint, levels::Method::Game, false); });
            break;
        }
    const bool loaded = WaitFor(25.0, [&] { return g_loadsFinished.load() > loads && !game::IsLoading(); });
    done = true;
    watchdog.join();
    if (!loaded) p.Problem("the level didn't load with the rendering thread off");
    else {
        const std::string why = WaitPlayerReady(20.0);
        if (!why.empty()) p.Problem("after loading: " + why);
        else s.notes.push_back("loaded with the rendering thread off");
    }
    p.Check(1, false);
    End(s);
}

// Memory snapshots (tas/snapshot.h), on a macro from a level start that runs, jumps and turns:
//   1. a playback keeps keyframes (the loader tracks the game's memory)
//   2. Go to an earlier frame puts the game back without loading the level: the player is exactly (bit for bit) where
//      the recording had them on that frame
//   3. playing on from there to the end gets exactly the same end state; keyframes reached again are compared
//   4. Frame back, ten times: each one exact, and how long one takes
//   5. Restart goes back to frame 0 without a level load
// Then the session stops and the game must still be playable.
void TestTasSnapshots()
{
    Step& s = Begin("TAS snapshots");
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
    auto* atEnd = tasModule ? dynamic_cast<ctl::Combo*>(tasModule->FindSetting("At the end")) : nullptr;
    auto* instant = tasModule ? dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Instant rewind")) : nullptr;
    if (!atEnd || !instant) return fail("the TAS module's At the end / Instant rewind controls were not found");
    const int atEndWas = atEnd->Get();
    const bool instantWas = instant->Get();

    std::string forward, jump;
    OnGameThread([&] {
        auto* pc = game::Controller();
        forward = KeyFor(pc, "GBA_MoveForward", "W");
        jump = KeyFor(pc, "GBA_Jump", "SpaceBar");
    });
    tas::Macro m(60.f);
    m.Describe("memenu self-test: snapshots").Wait(5).Press(forward).Wait(30);
    for (int i = 0; i < 20; ++i) m.Axis("MouseX", 6.f).Wait(1);     // turning while running
    m.Hold(jump, 3).Wait(50).Release(forward).Wait(40);
    tas::Replay macro = m.Build();
    macro.start = tas::StartKind::Checkpoint;
    macro.map = maps[index].label;
    macro.checkpoint = cp > 0 ? maps[index].checkpoints[static_cast<std::size_t>(cp - 1)] : std::string();
    macro.syncValidUntil = 0;                       // this playback records the reference states
    const std::size_t frames = macro.FrameCount();

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([&] {
        gui::visible = false;
        atEnd->Set(static_cast<int>(tas::AtEnd::Pause));
        instant->Set(true);
    });
    WaitTicks(3);

    const auto pausedOn = [](std::size_t frame) {
        const tas::Status st = tas::GetStatus();
        return st.mode == tas::Mode::Playing && st.paused && !st.seeking && st.frame == frame;
    };
    const auto describe = [](const tas::Sync& a) {
        char text[160];
        std::snprintf(text, sizeof text, "(%.3f, %.3f, %.3f) v (%.3f, %.3f, %.3f) move %u", a.x, a.y, a.z, a.vx, a.vy,
                      a.vz, a.state);
        return std::string(text);
    };
    const auto reference = [&](std::size_t frame) {
        tas::Sync want;
        tas::WithReplay([&](tas::Replay& r) { if (frame < r.sync.size()) want = r.sync[frame]; });
        return want;
    };
    // Goes to `frame` and checks the player against the recording; the milliseconds it took (-1: it didn't get there)
    const auto goTo = [&](std::size_t frame, bool back) -> double {
        const ULONGLONG t0 = GetTickCount64();
        if (back) tas::FrameBack(1);
        else tas::Goto(frame);
        if (!WaitFor(20.0, [&] { return pausedOn(frame); })) {
            const tas::Status st = tas::GetStatus();
            p.Problem("going to frame " + std::to_string(frame) + " stopped on " + std::to_string(st.frame) +
                      (st.paused ? " (paused)" : " (running)") + ": " + st.message);
            return -1.0;
        }
        const double ms = static_cast<double>(GetTickCount64() - t0);
        const tas::Sync now = tas::GetStatus().player;
        const tas::Sync want = frame < frames ? reference(frame) : now;
        if (frame < frames && !(now == want))
            p.Problem("frame " + std::to_string(frame) + ": the player is at " + describe(now) + ", recorded " +
                      describe(want));
        return ms;
    };

    // 1. The reference run, keyframes kept
    const int loadsBefore = g_loadsFinished.load();
    tas::Play();
    bool started = false;
    WaitFor(90.0, [&] {
        const tas::Status st = tas::GetStatus();
        started |= st.mode != tas::Mode::Idle;
        return pausedOn(frames) || (started && st.mode == tas::Mode::Idle);
    });
    tas::Status end = tas::GetStatus();
    if (!pausedOn(frames)) {
        tas::Stop();
        tas::WithReplay([&](tas::Replay& r) { r = saved; });
        OnRenderThread([&] { atEnd->Set(atEndWas); instant->Set(instantWas); gui::visible = true; });
        return fail("the playback didn't pause at its end (frame " + std::to_string(end.frame) + " of " +
                    std::to_string(frames) + "): " + end.message);
    }
    if (!end.snapshots) {
        tas::Stop();
        tas::WithReplay([&](tas::Replay& r) { r = saved; });
        OnRenderThread([&] { atEnd->Set(atEndWas); instant->Set(instantWas); gui::visible = true; });
        return fail("no snapshots: " + end.snapshotsOff);
    }
    const tas::Sync endPlayer = end.player;
    char line[200];
    std::snprintf(line, sizeof line, "%zu frames, %zu keyframes, %.1f MB stored; base %.0f ms, last keyframe %.1f ms",
                  frames, end.keyframes, static_cast<double>(end.snapshotBytes) / 1048576.0, end.snapshotBeginMs,
                  end.captureMs);
    s.notes.push_back(line);
    const int loadsAfterPlay = g_loadsFinished.load();
    // 2. Back to an earlier frame
    const double backMs = goTo(37, false);
    if (backMs >= 0) {
        std::snprintf(line, sizeof line, "went back to frame 37 in %.0f ms (restore %.1f ms), exactly as recorded", backMs,
                      tas::GetStatus().restoreMs);
        s.notes.push_back(line);
    }

    // 3. On to the end again: the same end state
    const double fwdMs = goTo(frames, false);
    if (fwdMs >= 0) {
        const tas::Status st = tas::GetStatus();
        if (!(st.player == endPlayer))
            p.Problem("playing on from 37 ended at " + describe(st.player) + ", the first time " + describe(endPlayer));
        std::snprintf(line, sizeof line, "played on to the end in %.0f ms: %s; %zu keyframes reached again differed "
                      "(%zu pages, put back)", fwdMs, st.player == endPlayer ? "same end state" : "DIFFERENT end state",
                      st.snapped, st.snappedPages);
        s.notes.push_back(line);
    }

    // 4. Frame back, ten times
    double total = 0.0, worst = 0.0;
    int steps = 0;
    for (std::size_t f = frames; f > frames - 10 && !g_aborted; --f) {
        const double ms = goTo(f - 1, true);
        if (ms < 0) break;
        total += ms;
        worst = std::max(worst, ms);
        ++steps;
    }
    if (steps) {
        std::snprintf(line, sizeof line, "frame back x%d: %.0f ms each on average, %.0f ms at most", steps,
                      total / steps, worst);
        s.notes.push_back(line);
    }

    // 4b. Early keyframes, the frames right after the level loaded, before frame 0 itself
    for (std::size_t f : {std::size_t{12}, std::size_t{6}}) {
        const double ms = goTo(f, false);
        if (ms >= 0) s.notes.push_back("went back to frame " + std::to_string(f));
        else break;
    }

    // 5. Restart: frame 0 without loading the level
    tas::Restart();
    if (!WaitFor(10.0, [] { const auto st = tas::GetStatus(); return st.mode == tas::Mode::Playing && st.frame <= 3 && !st.armed; }))
        p.Problem("Restart didn't go back to frame 0 (frame " + std::to_string(tas::GetStatus().frame) + ")");
    else if (g_loadsFinished.load() != loadsAfterPlay)
        p.Problem("Restart loaded the level instead of putting frame 0 back");
    else
        s.notes.push_back("restart: back at frame 0 without loading the level");
    (void)loadsBefore;

    // 6. A small memory budget: older keyframes are thinned out, and going back there is still exact (from an earlier
    // keyframe, running the frames between)
    auto* budget = dynamic_cast<ctl::Slider*>(tasModule->FindSetting("Memory"));
    if (budget) {
        const float budgetWas = budget->Get();
        OnRenderThread([&] { budget->Set(256.f); });
        WaitFor(90.0, [&] { return pausedOn(frames); });
        const tas::Status thin = tas::GetStatus();
        std::snprintf(line, sizeof line, "with 256 MB: %zu keyframes, %.1f MB", thin.keyframes,
                      static_cast<double>(thin.snapshotBytes) / 1048576.0);
        s.notes.push_back(line);
        if (thin.snapshotBytes > end.snapshotBytes) p.Problem("the budget didn't thin any keyframe out");
        for (std::size_t f : {std::size_t{10}, std::size_t{20}, std::size_t{100}})
            if (goTo(f, false) < 0) break;
        s.notes.push_back("went back to frames 10, 20 and 100 after thinning");
        OnRenderThread([&] { budget->Set(budgetWas); });
    } else {
        p.Problem("the TAS module's Memory control was not found");
    }

    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");
    if (tas::GetStatus().snapshots) p.Problem("the snapshots weren't let go of when the session stopped");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        atEnd->Set(atEndWas);
        instant->Set(instantWas);
        gui::visible = true;
    });
    if (const std::string why = WaitPlayerReady(); !why.empty()) p.Problem("after the session: " + why);
    WaitTicks(10);
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

#if MEMENU_FEATURE_TAS
// Not a check: measures what memory snapshots of the game would hold (opt-in: --only "TAS memory survey"). How much of
// the process is the game's, how much of it changes in one frame and in a second, which threads there are and what
// switching the rendering thread off does. Everything goes to the notes and the log.
void TestTasMemorySurvey()
{
    using namespace tas::memmap;
    Step& s = Begin("TAS memory survey");
    Probe p(s);
    const auto note = [&](std::string text) {
        spdlog::info("Survey: {}", text);
        s.notes.push_back(std::move(text));
    };
    const auto mb = [](std::uint64_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MB"; };
    if (!Tracking()) note("the allocation tracker is not running: virtual memory can't be attributed");

    // Threads
    const auto threadLines = [&](const char* when) {
        std::map<std::string, int> byModule;
        for (const auto& t : Threads()) {
            ++byModule[t.startModule];
            spdlog::info("Survey: thread {} start {}+{:#x} state {} wait {} stack {:#x}-{:#x} cycles {}", t.id,
                         t.startModule, t.start, t.state, t.waitReason, t.stackLow, t.stackHigh, t.cycles);
        }
        std::string line = std::string("threads ") + when + ":";
        for (const auto& [m, n] : byModule) line += " " + m + " x" + std::to_string(n);
        note(line);
    };
    threadLines("now");

    // The game's own threads: how busy each is over a second, and where it is (a few stack samples)
    {
        std::map<std::uint32_t, std::uint64_t> cycles0;
        for (const auto& t : Threads()) cycles0[t.id] = t.cycles;
        Wait(1.0);
        for (const auto& t : Threads()) {
            if (t.startModule != "mirrorsedge.exe" && t.startModule.rfind("physx", 0) != 0) continue;
            const double mcycles = static_cast<double>(t.cycles - cycles0[t.id]) / 1e6;
            note("thread " + std::to_string(t.id) + " (" + t.startModule + ") " + std::to_string(static_cast<int>(mcycles)) +
                 " Mcycles/s, state " + std::to_string(t.state) + " wait " + std::to_string(t.waitReason));
            if (tas::engine::IsGameThread()) continue;
            for (int sample = 0; sample < 3; ++sample) {
                std::string line = "  ";
                for (const auto& w : StackSample(t.id, 10)) line += w + " ";
                spdlog::info("Survey: thread {} sample {}: {}", t.id, sample, line);
                if (sample == 0) s.notes.push_back(line);
                Wait(0.05);
            }
        }
    }
    // Every heap: its size, and every module whose data holds its handle (who uses it)
    {
        const auto modules = Modules();
        for (const auto& h : Heaps()) {
            std::uint64_t bytes = 0, blocks = 0;
            if (HeapLock(h.handle)) {
                PROCESS_HEAP_ENTRY e{};
                while (HeapWalk(h.handle, &e))
                    if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) { bytes += e.cbData; ++blocks; }
                HeapUnlock(h.handle);
            }
            std::string refs;
            const auto value = reinterpret_cast<std::uintptr_t>(h.handle);
            for (const auto& m : modules) {
                for (std::uintptr_t q = m.base; q < m.base + m.size;) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (!VirtualQuery(reinterpret_cast<void*>(q), &mbi, sizeof mbi)) break;
                    const auto lo = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                    const auto hi = std::min<std::uintptr_t>(lo + mbi.RegionSize, m.base + m.size);
                    bool found = false;
                    if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD))
                        for (auto a = lo; a + 4 <= hi && !found; a += 4)
                            found = *reinterpret_cast<const std::uintptr_t*>(a) == value;
                    if (found) { refs += " " + m.name; break; }
                    q = hi;
                }
            }
            char text[256];
            std::snprintf(text, sizeof text, "heap %p %s%s: %llu KB in %llu blocks; referenced by:%s", h.handle,
                          h.what.c_str(), h.game ? " (game)" : "", static_cast<unsigned long long>(bytes / 1024),
                          static_cast<unsigned long long>(blocks), refs.empty() ? " nobody" : refs.c_str());
            note(text);
        }
    }

    // Regions by owner, and the biggest kinds
    std::vector<Region> regions;
    double regionsMs = 0.0;
    OnGameThread([&] {
        const ULONGLONG t0 = GetTickCount64();
        regions = Regions();
        regionsMs = static_cast<double>(GetTickCount64() - t0);
    }, 30.0);
    std::map<std::string, std::uint64_t> byOwner, byWhat;
    for (const auto& r : regions) {
        byOwner[OwnerName(r.owner)] += r.size;
        byWhat[std::string(OwnerName(r.owner)) + ": " + r.what] += r.size;
    }
    std::string owners = "writable memory (" + std::to_string(regions.size()) + " regions, listed in " +
                         std::to_string(static_cast<int>(regionsMs)) + " ms):";
    for (const auto& [o, b] : byOwner) owners += " " + o + " " + mb(b) + ";";
    note(owners);
    std::vector<std::pair<std::uint64_t, std::string>> whats;
    for (const auto& [w, b] : byWhat) whats.emplace_back(b, w);
    std::sort(whats.rbegin(), whats.rend());
    for (std::size_t i = 0; i < whats.size() && i < 25; ++i) note("  " + whats[i].second + ": " + mb(whats[i].first));

    std::string callers = "VirtualAlloc callers since the TAS started (bytes, live):";
    int shown = 0;
    for (const auto& c : AllocationCallers())
        if (c.bytes && shown++ < 12) callers += " " + c.module + " " + mb(c.bytes) + "/" + mb(c.live) + ";";
    note(callers);

    // How much changes: page hashes of the game's memory and of the unattributed memory, now, a frame later, a second later
    using Hashes = std::unordered_map<std::uintptr_t, std::pair<std::uint64_t, std::uint8_t>>;   // page -> hash, kind
    const auto kindOf = [](const Region& r) -> int {
        if (r.owner == Owner::GameImage) return 0;
        if (r.owner == Owner::GameHeap) return 1;
        if (r.owner == Owner::GameVirtual) return 2;
        if (r.what == "virtual (unknown)") return 3;
        return -1;
    };
    const char* kindNames[] = {"game image", "game heap", "game virtual", "unknown virtual"};
    const auto hashAll = [&](Hashes& out, double& ms) {
        OnGameThread([&] {
            const ULONGLONG t0 = GetTickCount64();
            out.clear();
            for (const auto& r : Regions()) {
                const int kind = kindOf(r);
                if (kind < 0) continue;
                for (std::uintptr_t a = r.base; a < r.base + r.size; a += kPage) {
                    std::uint64_t h = 0;
                    if (HashPage(reinterpret_cast<const void*>(a), h)) out[a] = {h, static_cast<std::uint8_t>(kind)};
                }
            }
            ms = static_cast<double>(GetTickCount64() - t0);
        }, 60.0);
    };
    const auto compare = [&](const Hashes& a, const Hashes& b, const char* when) {
        std::uint64_t changed[4]{}, total[4]{}, added[4]{};
        for (const auto& [page, hk] : b) {
            total[hk.second] += kPage;
            const auto it = a.find(page);
            if (it == a.end()) added[hk.second] += kPage;
            else if (it->second.first != hk.first) changed[hk.second] += kPage;
        }
        std::string line = std::string("changed ") + when + ":";
        for (int k = 0; k < 4; ++k)
            line += std::string(" ") + kindNames[k] + " " + std::to_string(changed[k] / 1024) + " KB of " + mb(total[k]) +
                    " (+" + std::to_string(added[k] / 1024) + " KB new);";
        note(line);
    };
    Hashes h0, h1, h2;
    double ms0 = 0, ms1 = 0, ms2 = 0;
    hashAll(h0, ms0);
    WaitTicks(1);
    hashAll(h1, ms1);
    compare(h0, h1, "over 1 tick");
    WaitTicks(62);
    hashAll(h2, ms2);
    compare(h1, h2, "over 62 ticks");
    note("hashing took " + std::to_string(static_cast<int>(ms0)) + " / " + std::to_string(static_cast<int>(ms1)) + " / " +
         std::to_string(static_cast<int>(ms2)) + " ms");

    // The rendering thread: off and back on with the engine's own console command
    std::set<std::uint32_t> before, after;
    for (const auto& t : Threads()) before.insert(t.id);
    OnGameThread([] { game::ConsoleCommand("ToggleRenderingThread"); });
    WaitTicks(10);
    for (const auto& t : Threads()) after.insert(t.id);
    std::string gone, added;
    for (auto id : before) if (!after.count(id)) gone += " " + std::to_string(id);
    for (auto id : after) if (!before.count(id)) added += " " + std::to_string(id);
    note("ToggleRenderingThread: threads gone [" + gone + " ] new [" + added + " ]");
    threadLines("after the toggle");
    Hashes r0, r1;
    double rm0 = 0, rm1 = 0;
    hashAll(r0, rm0);
    WaitTicks(1);
    hashAll(r1, rm1);
    compare(r0, r1, "over 1 tick without the rendering thread");
    OnGameThread([] { game::ConsoleCommand("ToggleRenderingThread"); });
    WaitTicks(10);
    std::set<std::uint32_t> back;
    for (const auto& t : Threads()) back.insert(t.id);
    note("toggled back: " + std::to_string(back.size()) + " threads (" + std::to_string(before.size()) + " before)");

    p.Check(1, true);
    End(s);
}
#endif

#if MEMENU_FEATURE_COLLISION_VIEW
// The collision view's inspector: with the panel open it lists the volumes near the player; facing one of them (the
// Nearby list's click) must make the look-at report that same volume: as the one looked at, or on the line behind
// another volume that hides it (the "behind" list). Volumes the player is inside can't be checked that way: up to five
// are tried, and one has to match.
// Back on the ground and walking (or 10 s passed). The kick tests jump and fall: in fast mode a tick count is almost
// no game time, so the next test would start with the player still in the air.
void WaitGrounded()
{
    WaitFor(10.0, [] {
        bool ok = false;
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            ok = pawn && pawn->Physics == Classes::EPhysics::PHYS_Walking &&
                 pawn->MovementState != Classes::EMovement::MOVE_Jump;
        }, 1.0);
        return ok;
    });
    WaitTicks(5);
}

#if MEMENU_FEATURE_KICKGLITCH_HELPER
// A kick is faked (the wallrun kick's move state on a player standing on the ground: "landed" the next tick), then the
// game is told to jump like the Kickglitch tool does. The helper must count one attempt and time the jump.
void TestKickglitchHelper()
{
    Step& s = Begin("Kickglitch helper");
    Probe p(s);
    Module* helper = modules::Find("Kickglitch Helper");
    if (!helper) {
        p.Problem("the Kickglitch Helper module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = helper->Enabled();
    WaitGrounded();
    OnRenderThread([&] { helper->SetEnabled(false); helper->SetEnabled(true); });
    WaitTicks(3);

    // The game's real kick move can't be started without a wall (in the open it never ends, and the game then ignores
    // the jump), so only its state is shown, for a few ticks of a real fall. The Kickglitch tool is armed in the air (it
    // does nothing while falling), so on the game's own landing it jumps on the first walking frame, as after a real
    // kick: the helper must time that jump and take it as its calibration.
    Module* tool = modules::Find("Kickglitch");
    auto* frame = dynamic_cast<ctl::SliderInt*>(helper->FindSetting("Perfect frame"));
    const int frameWas = frame ? frame->Get() : 1;
    if (frame) OnRenderThread([&] { frame->Set(5); });   // Must be overwritten by the calibration
    OnGameThread([tool] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        pawn->Location.Z += 150.f;
        pawn->Velocity = Classes::FVector{};
        pawn->Physics = Classes::EPhysics::PHYS_Falling;
        mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
        if (tool) tool->SetEnabled(true);
    });
    WaitTicks(1);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_MeleeWallrun; });
    WaitTicks(2);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_Falling; });
    WaitFor(5.0, [] { return kickglitch_helper::Get().attempts > 0; });

    const auto last = kickglitch_helper::Get();
    if (last.attempts != 1) p.Problem("it counted " + std::to_string(last.attempts) + " attempts, not 1");
    else if (last.jumpFrame < 0) p.Problem("it saw the kick and the landing but not the jump");
    else s.notes.push_back("jump timed " + std::to_string(last.jumpFrame) + " frame(s) after landing (the Kickglitch tool's jump)");
    if (!tool) p.Problem("the Kickglitch tool was not found (no calibration)");
    else if (last.jumpFrame >= 0 && !last.calibrated)
        p.Problem("the attempt was not taken as the Kickglitch tool's (no calibration)");
    else if (frame && last.jumpFrame >= 0 && frame->Get() != std::clamp(last.jumpFrame, 0, 5))
        p.Problem("the tool's attempt did not calibrate the perfect frame (" + std::to_string(frame->Get()) + ")");

    OnRenderThread([&] {
        if (tool) tool->SetEnabled(false);
        if (frame) frame->Set(frameWas);
        helper->SetEnabled(enabledWas);
    });
    OnGameThread([] {
        if (auto* pawn = game::Pawn(); pawn && game::Controller()) player_state::ResetMovement(pawn, game::Controller());
    });
    WaitGrounded();     // Before the next test
    p.Check(1, false);
    End(s);
}
#endif

#if MEMENU_FEATURE_AUTO_KICKGLITCH
// Same faked kick as the helper's test (the kick's state for a few ticks of a real fall): on the game's landing the
// module must jump by itself, with nothing else armed.
void TestAutoKickglitch()
{
    Step& s = Begin("Auto kickglitch");
    Probe p(s);
    Module* autoKg = modules::Find("Auto Kickglitch");
    if (!autoKg) {
        p.Problem("the Auto Kickglitch module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = autoKg->Enabled();
    WaitGrounded();     // A test before may have left the player in the air
    OnRenderThread([&] { autoKg->SetEnabled(true); });
    OnGameThread([] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        pawn->Location.Z += 150.f;
        pawn->Velocity = Classes::FVector{};
        pawn->Physics = Classes::EPhysics::PHYS_Falling;
        mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
    });
    WaitTicks(1);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_MeleeWallrun; });
    WaitTicks(2);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_Falling; });

    // Landed, then up again on its own: the jump
    bool landed = false, jumped = false;
    WaitFor(5.0, [&] {
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            if (pawn->Physics == Classes::EPhysics::PHYS_Walking) landed = true;
            else if (landed && (pawn->MovementState == Classes::EMovement::MOVE_Jump || pawn->Velocity.Z > 100.f))
                jumped = true;
        }, 1.0);
        return jumped;
    });
    if (!landed) p.Problem("the player never landed");
    else if (!jumped) p.Problem("no jump after the kick's landing");

    // The kick buffer: a kick pressed in the air (the console runs AttackPress the way a key binding does) is held
    // back, then let go late when no wall comes within the window. The wall kick itself needs a real wall.
    auto* buffer = dynamic_cast<ctl::Checkbox*>(autoKg->FindSetting("Kick buffer"));
    auto* window = dynamic_cast<ctl::SliderInt*>(autoKg->FindSetting("Buffer window"));
    auto* onMiss = dynamic_cast<ctl::Combo*>(autoKg->FindSetting("No wall in time"));
    if (!buffer || !window || !onMiss) {
        p.Problem("the kick buffer settings were not found");
    } else {
        const bool bufferWas = buffer->Get();
        const int windowWas = window->Get(), onMissWas = onMiss->Get();
        OnRenderThread([&] {
            buffer->Set(true);
            window->Set(100);
            onMiss->Set(0);         // Kick anyway
        });
        WaitGrounded();
        const auto before = auto_kickglitch::Stats();
        OnGameThread([] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            pawn->Location.Z += 400.f;
            pawn->Velocity = Classes::FVector{};
            pawn->Physics = Classes::EPhysics::PHYS_Falling;
            mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
        });
        WaitTicks(2);
        bool airKick = false;
        OnGameThread([&] {
            game::ConsoleCommand("AttackPress");
            game::ConsoleCommand("AttackRelease");
            if (auto* pawn = game::Pawn()) airKick = pawn->MovementState == Classes::EMovement::MOVE_MeleeAir;
        });
        WaitTicks(1);
        const auto held = auto_kickglitch::Stats();
        WaitFor(3.0, [&] {
            const auto st = auto_kickglitch::Stats();
            return st.late + st.dropped > before.late + before.dropped;
        });
        const auto after = auto_kickglitch::Stats();
        char note[160];
        std::snprintf(note, sizeof note, "kick buffer: hooked %s, held %d, let go late %d, dropped %d",
                      after.hooked ? "yes" : "no", after.held - before.held, after.late - before.late,
                      after.dropped - before.dropped);
        s.notes.push_back(note);
        if (!after.hooked) p.Problem("the kick press can't be held back (AttackPress not watched)");
        else if (held.held <= before.held) p.Problem("a kick pressed in the air was not held back");
        else if (airKick) p.Problem("the held kick still kicked in the air at once");
        else if (after.late <= before.late) p.Problem("the held kick was not let go when no wall came");
        OnRenderThread([&] {
            buffer->Set(bufferWas);
            window->Set(windowWas);
            onMiss->Set(onMissWas);
        });
    }

    OnRenderThread([&] { autoKg->SetEnabled(enabledWas); });
    OnGameThread([] {
        if (auto* pawn = game::Pawn(); pawn && game::Controller()) player_state::ResetMovement(pawn, game::Controller());
    });
    WaitGrounded();     // Before the next test
    p.Check(1, false);
    End(s);
}
#endif

#if MEMENU_FEATURE_KILL_BOTS
// Only checks the kills where the level has enemies (start game-test with --map at one); elsewhere, that it runs.
void TestKillBots()
{
    Step& s = Begin("Kill bots");
    Probe p(s);
    Module* kill = modules::Find("Kill Bots");
    if (!kill) {
        p.Problem("the Kill Bots module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const auto alive = [] {
        int n = 0;
        OnGameThread([&] {
            auto* world = game::World();
            for (Classes::APawn* pawn = world ? world->PawnList : nullptr; pawn; pawn = pawn->NextPawn)
                if (pawn != game::Pawn() && game::IsA(pawn, Classes::ATdBotPawn::StaticClass()) && pawn->Health > 0 &&
                    !pawn->bDeleteMe)
                    ++n;
        });
        return n;
    };
    const int before = alive();
    OnRenderThread([&] { kill->Toggle(); });
    WaitTicks(10);
    const int after = alive();
    if (before == 0) s.notes.push_back("no enemies in this level: only checked that it runs");
    else if (after > 0) p.Problem(std::to_string(after) + " of " + std::to_string(before) + " enemies still alive");
    else s.notes.push_back("killed " + std::to_string(before) + (before == 1 ? " enemy" : " enemies"));
    p.Check(1, false);
    End(s);
}
#endif

// ---- Engine hooks ---------------------------------------------------------------------------------------------------
Step& BeginEngine(const char* name, Probe*& probe, std::unique_ptr<Probe>& holder)
{
    Step& s = Begin(name);
    holder = std::make_unique<Probe>(s);
    probe = holder.get();
    return s;
}

// Strang is the state handler hook's user: armed, a new move (falling, here) must make it jump and disarm itself.
void TestEngineStateHook()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine state hook", p, holder);
    Module* strang = modules::Find("Strang");
    if (!game::PlayerStateHooked()) p->Problem("the player state handler is not hooked");
    else if (!strang) p->Problem("the Strang module was not found");
    else {
        OnRenderThread([&] { strang->SetEnabled(true); });
        WaitTicks(2);
        OnGameThread([] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            pawn->Location.Z += 150.f;
            pawn->Velocity = Classes::FVector{};
            pawn->Physics = Classes::EPhysics::PHYS_Falling;
            mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
        });
        if (!WaitFor(3.0, [&] { return !strang->Enabled(); })) p->Problem("Strang was still armed after a new move began");
        else s.notes.push_back("Strang fired on the state change");
        OnRenderThread([&] { strang->SetEnabled(false); });
        WaitGrounded();     // Strang jumped
    }
    p->Check(1, false);
    End(s);
}

std::atomic<int> g_tickBefore{0}, g_tickAfter{0};
std::atomic<bool> g_blockTick{false};

// Before, after and blocking watches on the controller's PlayerTick, an event the engine calls every tick.
void TestEngineScriptEvents()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine script events", p, holder);
    static bool added = false;
    if (!added) {
        added = true;
        for (const char* fn : {"Function TdGame.TdPlayerController.PlayerTick", "Function Engine.PlayerController.PlayerTick"}) {
            game::OnScriptEvent(fn, [](Classes::UObject*, void*) { ++g_tickBefore; });
            game::OnScriptEvent(fn, [](Classes::UObject*, void*) { ++g_tickAfter; }, true);
            game::BlockScriptEvent(fn, [](Classes::UObject*, void*) { return g_blockTick.load(); });
        }
    }
    WaitTicks(10);      // Looked up on the next tick
    const int b0 = g_tickBefore, a0 = g_tickAfter;
    WaitTicks(10);
    const int b1 = g_tickBefore, a1 = g_tickAfter;
    if (b1 <= b0) p->Problem("the before-watch on PlayerTick never ran");
    else if (a1 <= a0) p->Problem("the after-watch on PlayerTick never ran");
    else {
        g_blockTick = true;
        WaitTicks(2);
        const int b2 = g_tickBefore, a2 = g_tickAfter;
        WaitTicks(5);
        const int b3 = g_tickBefore, a3 = g_tickAfter;
        g_blockTick = false;
        WaitTicks(5);
        const int a4 = g_tickAfter;
        if (b3 <= b2) p->Problem("while blocked, PlayerTick stopped being called at all");
        else if (a3 != a2) p->Problem("blocked, PlayerTick still ran (its after-watch did)");
        else if (a4 <= a3) p->Problem("PlayerTick did not run again after unblocking");
        else s.notes.push_back("before/after/block all work (" + std::to_string(b1 - b0) + " calls in 10 ticks)");
    }
    p->Check(1, false);
    End(s);
}

#if MEMENU_FEATURE_FRAME_RATE
// Frame Rate Limit: on, the engine's cap is the module's; off, the engine has exactly what it had before (the game's 62,
// or the test's own uncapped fast mode): normal play is never left at another frame rate.
void TestFrameRate()
{
    Step& s = Begin("Frame rate limit");
    Probe p(s);
    Module* fps = modules::Find("Frame Rate Limit");
    auto* cap = fps ? dynamic_cast<ctl::Slider*>(fps->FindSetting("Cap")) : nullptr;
    auto* uncapped = fps ? dynamic_cast<ctl::Checkbox*>(fps->FindSetting("Uncapped")) : nullptr;
    if (!fps || !cap || !uncapped) {
        p.Problem("the Frame Rate Limit module or its settings were not found");
        p.Check(1, false);
        End(s);
        return;
    }
    struct Cap { bool smooth = false; float max = 0.f; bool found = false; };
    const auto read = [] {
        Cap c;
        OnGameThread([&] {
            const auto& objects = Classes::UObject::GetGlobalObjects();
            for (std::size_t i = 0; i < objects.Num(); ++i) {
                auto* o = objects.GetByIndex(i);
                if (!o || !game::IsA(o, Classes::UGameEngine::StaticClass()) || o->GetName().rfind("Default__", 0) == 0)
                    continue;
                auto* e = static_cast<Classes::UGameEngine*>(o);
                c = {e->bSmoothFrameRate != 0, e->MaxSmoothedFrameRate, true};
                break;
            }
        });
        return c;
    };
    const bool enabledWas = fps->Enabled(), uncappedWas = uncapped->Get();
    const float capWas = cap->Get();
    if (enabledWas) {
        OnRenderThread([&] { fps->SetEnabled(false); });
        WaitTicks(3);
    }
    const Cap before = read();
    if (!before.found) p.Problem("no game engine object");

    OnRenderThread([&] {
        cap->Set(30.f);
        uncapped->Set(false);
        fps->SetEnabled(true);
    });
    WaitTicks(5);
    const Cap on = read();
    OnRenderThread([&] { fps->SetEnabled(false); });
    WaitTicks(5);
    const Cap after = read();

    char note[200];
    std::snprintf(note, sizeof note, "before: cap %s %.0f; module at 30: %s %.0f; off again: %s %.0f",
                  before.smooth ? "on" : "off", before.max, on.smooth ? "on" : "off", on.max,
                  after.smooth ? "on" : "off", after.max);
    s.notes.push_back(note);
    if (!on.smooth || std::fabs(on.max - 30.f) > 0.01f) p.Problem("the module's 30 fps cap was not set on the engine");
    if (after.smooth != before.smooth || std::fabs(after.max - before.max) > 0.01f)
        p.Problem("switching the module off did not put the engine's cap back");

    OnRenderThread([&] {
        cap->Set(capWas);
        uncapped->Set(uncappedWas);
        fps->SetEnabled(enabledWas);
    });
    WaitTicks(3);
    p.Check(1, false);
    End(s);
}
#endif

// "set" on the engine object, read back from the object itself.
// Mirage step 1: UnrealLib reads the game's script packages inside the game, and Lua runs, without stopping the game
// (the parse runs on this thread while the game keeps ticking and drawing), within a time and memory budget.
void TestMirageRuntime()
{
    constexpr double kMaxSeconds = 20.0;
    constexpr std::int64_t kMaxPeakBytes = 512ll << 20;
    // Still allocated after the release, on a second run (the first keeps one-time setup): counted from the menu's own
    // allocations (runtime/memory.h), so only the menu's other threads add a little noise. A leak repeats every run.
    constexpr std::int64_t kMaxRetainedBytes = 1ll << 20;

    Step& s = Begin("Mirage runtime");
    Probe p(s);
    const mirage::packages::Report pk = mirage::packages::Probe();
    if (!pk.ok) {
        p.Problem("UnrealLib: " + pk.error);
    } else {
        for (const auto& pkg : pk.packages)
            s.notes.push_back(pkg.name + ": " + std::to_string(pkg.exports) + " exports, " + std::to_string(pkg.classes) +
                              " classes, " + std::to_string(pkg.functions) + " functions");
        char line[160];
        std::snprintf(line, sizeof line, "UnrealLib: %zu classes, %zu properties in %.2f s, +%.0f MB loaded, +%.1f MB after",
                      pk.classesRead, pk.properties, pk.seconds, static_cast<double>(pk.peakBytes) / 1048576.0,
                      static_cast<double>(pk.retainedBytes) / 1048576.0);
        s.notes.push_back(line);
        if (pk.seconds > kMaxSeconds)
            p.Problem("reading the script packages took " + std::to_string(pk.seconds) + " s (budget " +
                      std::to_string(kMaxSeconds) + " s)");
        const mirage::packages::Report again = mirage::packages::Probe();
        std::snprintf(line, sizeof line, "second run: %.2f s, +%.0f MB loaded, +%.1f MB after", again.seconds,
                      static_cast<double>(again.peakBytes) / 1048576.0, static_cast<double>(again.retainedBytes) / 1048576.0);
        s.notes.push_back(line);
        const std::int64_t peak = std::max(pk.peakBytes, again.ok ? again.peakBytes : 0);
        if (peak > kMaxPeakBytes)
            p.Problem("reading the script packages used " + std::to_string(peak >> 20) + " MB (budget " +
                      std::to_string(kMaxPeakBytes >> 20) + " MB)");
        if (!again.ok) p.Problem("UnrealLib, second run: " + again.error);
        else if (again.retainedBytes > kMaxRetainedBytes)
            p.Problem("a second run left " + std::to_string(again.retainedBytes >> 10) +
                      " KB more allocated after UnrealLib released everything: a leak");
    }

    const mirage::lua::ProbeReport lua = mirage::lua::Probe();
    for (const std::string& problem : lua.problems) p.Problem("Lua: " + problem);
    if (lua.ok)
        s.notes.push_back(std::string(mirage::lua::Version()) + ": " + std::to_string(lua.notes.size()) + " checks passed");

    p.Check(1, true);       // The game kept drawing and ticking meanwhile
    End(s);
}

// Mirage step 2: the layouts UnrealLib computes from the script packages are the engine's own, for every class, struct,
// function and state of Core, Engine and TdGame. Every difference goes to memenu\mirage-layouts.txt.
// Mirage step 2: calls with the engine's own parameter layout, native functions with a native index included (the SDK's
// IsInState and SetLocation answered garbage: ProcessEvent refuses those, and its bool parameters were 1 byte).
void TestMirageCalls()
{
    Step& s = Begin("Mirage calls");
    Probe p(s);
    std::vector<std::string> problems;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) { problems.push_back("no player"); return; }
        std::string why;
        if (!mirage::call::LayoutChecked(&why)) { problems.push_back("UFunction layout: " + why); return; }
        const auto call = [&](Classes::UObject* o, mirage::call::Params& params, const char* what) {
            std::string error;
            if (!params.Valid() || !mirage::call::Call(o, params, &error))
                problems.push_back(std::string(what) + ": " + (params.Valid() ? error : params.Error()));
        };

        // IsInState (native 281), against the state read from the state frame
        for (const char* state : {"PlayerWalking", "Dead"}) {
            mirage::call::Params is(pc, "IsInState");
            is.SetName("TestState", state);
            is.Set("bTestStateStack", false);
            call(pc, is, "IsInState");
            const bool want = game::IsInState(pc, state);
            if (is.GetBool("ReturnValue") != want)
                problems.push_back(std::string("IsInState(") + state + ") answered " + (want ? "false" : "true"));
        }

        // SetLocation (native 267): the player really moves
        const Classes::FVector before = pawn->Location;
        Classes::FVector target = before;
        target.Z += 64.f;
        mirage::call::Params at(pawn, "SetLocation");
        at.SetRaw("NewLocation", &target, sizeof target);
        call(pawn, at, "SetLocation");
        const float moved = pawn->Location.Z - before.Z;
        if (!at.GetBool("ReturnValue")) problems.push_back("SetLocation returned false");
        else if (std::fabs(moved - 64.f) > 1.f)
            problems.push_back("SetLocation returned true but the player moved " + std::to_string(moved) + " up, not 64");
        pawn->Location = before;

        // SetMove (script, two bools before a bool return value). It returns false for the move the player is already in
        // (`if (NewMove == MovementState) return false;`), true for a change: both answers must come back right.
        const auto setMove = [&](Classes::EMovement m) {
            mirage::call::Params move(pawn, "SetMove");
            move.Set("NewMove", std::uint8_t{static_cast<std::uint8_t>(m)});
            move.Set("bViaReplication", false);
            move.Set("bCheckCanDo", false);
            call(pawn, move, "SetMove");
            return move.GetBool("ReturnValue");
        };
        using Classes::EMovement;
        if (pawn->MovementState == EMovement::MOVE_Walking && setMove(EMovement::MOVE_Walking))
            problems.push_back("SetMove(Walking) while walking returned true (the script returns false)");
        if (!setMove(EMovement::MOVE_Crouch)) problems.push_back("SetMove(Crouch) returned false");
        else if (pawn->MovementState != EMovement::MOVE_Crouch) problems.push_back("SetMove(Crouch) did not crouch");
        if (!setMove(EMovement::MOVE_Walking)) problems.push_back("SetMove(Walking) after crouching returned false");
        else if (pawn->MovementState != EMovement::MOVE_Walking) problems.push_back("SetMove(Walking) did not walk");

        // Wrong types are refused, not written
        mirage::call::Params wrong(pc, "IsInState");
        if (wrong.Set("TestState", 1.f)) problems.push_back("a float was accepted for a name parameter");
        if (wrong.Set("NoSuchParameter", true)) problems.push_back("an unknown parameter was accepted");
        if (!problems.empty()) return;
        s.notes.push_back("IsInState, SetLocation (native) and SetMove (script) answered like the engine");
    }, 10.0);
    for (const auto& problem : problems) p.Problem(problem);
    WaitGrounded();
    p.Check(1, true);
    End(s);
}

void TestMirageLayouts()
{
    Step& s = Begin("Mirage layouts");
    Probe p(s);
    // Every package the SDK covers. The native-only ones have no .u file: Compute skips what it can't find
    const std::vector<std::string>& packages = mirage::layout::SdkPackages();
    std::vector<std::string> scripts;
    for (const auto& name : packages)
        if (std::filesystem::exists(mirage::packages::CookedDir() / (name + ".u"))) scripts.push_back(name);

    mirage::layout::Layouts live;
    const clk::Ticks t0 = clk::Now();
    if (!OnGameThread([&] { live = mirage::layout::Live(packages); }, 30.0)) p.Problem("reading the live layouts timed out");
    const double liveSeconds = clk::SecondsSince(t0);
    const clk::Ticks t1 = clk::Now();
    const mirage::layout::Computed computed = mirage::layout::Compute(mirage::packages::CookedDir(), scripts);
    const double computeSeconds = clk::SecondsSince(t1);
    if (!computed.error.empty()) p.Problem("UnrealLib: " + computed.error);

    const mirage::layout::Report r = mirage::layout::Compare(live, computed.layouts);
    char line[200];
    std::snprintf(line, sizeof line, "%zu live (%.2f s), %zu computed (%.2f s); %zu compared: %zu match; properties %zu of %zu",
                  live.size(), liveSeconds, computed.layouts.size(), computeSeconds, r.structs, r.structsMatching,
                  r.propertiesMatching, r.properties);
    s.notes.push_back(line);
    if (r.onlyLive || r.onlyComputed)
        s.notes.push_back(std::to_string(r.onlyLive) + " only in the engine, " + std::to_string(r.onlyComputed) +
                          " only computed");
    for (const auto& [cause, n] : r.byCause) s.notes.push_back("  " + std::to_string(n) + "x " + cause);

    // Everything into a file, to read the differences one by one
    const std::filesystem::path file = paths::Dir() / "mirage-layouts.txt";
    {
        std::ofstream out(file, std::ios::trunc);
        out << line << "\n\n";
        for (const auto& [cause, n] : r.byCause) out << n << "  " << cause << "\n";
        out << "\n";
        for (const auto& m : r.mismatches) out << m.path << ": " << m.what << "\n";
        out << "\nOnly in the engine:\n";
        for (const auto& path : r.onlyLivePaths) out << "  " << path << "\n";
        out << "Only computed:\n";
        for (const auto& path : r.onlyComputedPaths) out << "  " << path << "\n";
        // Both sides of the structs named in MEMENU_LAYOUT_DETAIL (comma separated paths), property by property
        out << "\n";
        char* detail = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&detail, &len, "MEMENU_LAYOUT_DETAIL") == 0 && detail) {
            std::string list = detail;
            std::free(detail);
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t comma = list.find(',', start);
                const std::string path = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                start = comma == std::string::npos ? list.size() + 1 : comma + 1;
                const mirage::layout::Layouts* const sides[] = {&live, &computed.layouts};
                for (const auto* side : sides) {
                    const auto it = side->find(path);
                    out << (side == &live ? "live     " : "computed ") << path;
                    if (it == side->end()) { out << ": none\n"; continue; }
                    char head[96];
                    std::snprintf(head, sizeof head, " (%s, size %#x, align %u, super %s)\n", it->second.kind.c_str(),
                                  it->second.size, it->second.alignment, it->second.super.c_str());
                    out << head;
                    for (const auto& prop : it->second.properties) {
                        char row[160];
                        std::snprintf(row, sizeof row, "    %#06x %-10s x%u size %-4u mask %#x  %s %s\n", prop.offset,
                                      mirage::layout::KindName(prop.kind), prop.arrayDim, prop.elementSize, prop.boolMask,
                                      prop.name.c_str(), prop.type.c_str());
                        out << row;
                    }
                }
            }
        }
    }
    // What only the engine knows (intrinsic classes, native-only packages): the SDK generator's second input
    {
        mirage::layout::Layouts onlyLive;
        for (const auto& [path, l] : live)
            if (!computed.layouts.count(path)) onlyLive.emplace(path, l);
        std::ofstream(paths::Dir() / "mirage-live-layouts.json", std::ios::trunc) << mirage::layout::ToJson(onlyLive);
        s.notes.push_back(std::to_string(onlyLive.size()) + " engine-only layouts saved to mirage-live-layouts.json");
    }
    // The generated SDK's checks the compiler can't make (each bool's bit in its dword; offsets and sizes are
    // static_asserts): its bitfields as MSVC packs them, against the masks the engine uses
    {
        std::string first;
        if (const int failures = Classes::detail::CheckLayouts(&first))
            p.Problem("the SDK has " + std::to_string(failures) + " bool bits where the engine doesn't (first: " +
                      first + ")");
        else
            s.notes.push_back("SDK bitfields match the engine");
    }
    if (!r.mismatches.empty())
        p.Problem(std::to_string(r.mismatches.size()) + " structs differ from the engine (first: " +
                  r.mismatches[0].path + ": " + r.mismatches[0].what + "); all in " + file.string());
    if (r.structs == 0) p.Problem("nothing was compared");

    p.Check(1, true);
    End(s);
}

void TestEngineConsole()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine console", p, holder);
    Classes::UEngine* engine = nullptr;
    bool was = false, now = false, ran = false;
    OnGameThread([&] {
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num() && !engine; ++i) {
            auto* o = objects.GetByIndex(i);
            if (o && game::IsA(o, Classes::UGameEngine::StaticClass()) && o->GetName().rfind("Default__", 0) != 0)
                engine = static_cast<Classes::UEngine*>(o);
        }
        if (!engine) return;
        was = engine->bEnableColorClear != 0;
        ran = game::ConsoleCommand(std::string("set Engine bEnableColorClear ") + (was ? "false" : "true"));
    });
    WaitTicks(2);
    if (engine) OnGameThread([&] { now = engine->bEnableColorClear != 0; });
    if (!engine) p->Problem("no engine object");
    else if (!ran) p->Problem("ConsoleCommand had no controller to run it");
    else if (now == was) p->Problem("\"set Engine bEnableColorClear\" did not change the engine");
    else s.notes.push_back("set changed bEnableColorClear");
    if (engine) OnGameThread([&] { game::ConsoleCommand(std::string("set Engine bEnableColorClear ") + (was ? "true" : "false")); });
    WaitTicks(2);
    p->Check(1, false);
    End(s);
}

std::atomic<int> g_deathBefore{0}, g_deathAfter{0};

// Kills the player: the death hooks must see it, and the game must bring the player back (checkpoint restart).
void TestEngineDeath()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine death hook", p, holder);
    static bool added = false;
    if (!added) {
        added = true;
        game::OnPlayerDeath([](bool after) { ++(after ? g_deathAfter : g_deathBefore); });
    }
    if (!game::DeathHooked()) {
        p->Problem("the death functions are not hooked");
        p->Check(1, false);
        End(s);
        return;
    }
    if (Module* god = modules::Find("God Mode")) OnRenderThread([god] { god->SetEnabled(false); });
    WaitTicks(2);
    const int before = g_deathBefore, after = g_deathAfter;
    const void* pawnBefore = nullptr;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) return;
        pawnBefore = pawn;
        Classes::FTraceHitInfo hit{};
        pawn->TakeDamage(pawn->Health + 10000, nullptr, pawn->Location, Classes::FVector{},
                         Classes::UTdDmgType_Fell::StaticClass(), hit, nullptr);
    });
#if MEMENU_FEATURE_TAS
    // With the rendering thread off (TAS snapshots) the hooked functions aren't reached: a respawn is the death then
    if (tas::snapshot::RenderingThreadOff()) {
        bool respawned = false;
        WaitFor(10.0, [&] {
            OnGameThread([&] { auto* pawn = game::Pawn(); respawned = pawn && pawn != pawnBefore && pawn->Health > 0; }, 1.0);
            return respawned;
        });
        if (!respawned) p->Problem("the player didn't die and come back (rendering thread off: the hook can't see it)");
        else s.notes.push_back("rendering thread off (TAS snapshots): death seen as a respawn, the hook isn't reached");
        WaitPlayerReady();
        p->Check(1, false);
        End(s);
        return;
    }
#endif
    if (!WaitFor(10.0, [&] { return g_deathBefore > before; })) {
        std::string state = "no player";
        OnGameThread([&] {
            if (auto* pawn = game::Pawn())
                state = "health " + std::to_string(pawn->Health) + ", physics " + std::to_string(static_cast<int>(pawn->Physics.GetValue())) +
                        ", move " + std::to_string(static_cast<int>(pawn->MovementState.GetValue())) +
                        (pawn == pawnBefore ? ", same pawn" : ", another pawn");
        }, 2.0);
        p->Problem("no death seen by the hook within 10 s (" + state + ")");
    }
    else {
        if (!WaitFor(10.0, [&] { return g_deathAfter > after; })) p->Problem("the after-death call never came");
        // Back in the game: a new pawn, alive and movable
        const bool back = WaitFor(60.0, [&] {
            bool ok = false;
            OnGameThread([&] { auto* pw = game::Pawn(); ok = pw && pw != pawnBefore && pw->Health > 0; }, 1.0);
            return ok;
        });
        // Walking again, with no cutscene or input hold. Not PlayerNotReady(): the engine leaves the controller's
        // bFrozen (set while dead; it only gates "fire to respawn") after the game's own checkpoint restart.
        bool walking = false, frozen = false;
        if (back)
            walking = WaitFor(30.0, [&] {
                OnGameThread([&] {
                    auto* pc = game::Controller();
                    walking = pc && game::Pawn() && game::IsInState(pc, "PlayerWalking") &&
                              !pc->bCinematicMode && !pc->bIgnoreMoveInput;
                    frozen = pc && pc->bFrozen;
                }, 1.0);
                return walking;
            });
        if (!back) p->Problem("the player did not respawn within 60 s");
        else if (!walking) p->Problem("after respawning, the player is not back in PlayerWalking");
        else s.notes.push_back(std::string("death seen before and after, player respawned and walking") +
                               (frozen ? " (bFrozen left set by the engine)" : ""));
    }
    p->Check(1, false);
    End(s);
}

#if MEMENU_FEATURE_KEY_BINDINGS
// The game's TdInput.ini (SetBind saves it): kept as it was before the test and written back after.
std::filesystem::path TdInputIni()
{
    wchar_t docs[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) return {};
    return std::filesystem::path(docs) / L"EA Games" / L"Mirror's Edge" / L"TdGame" / L"Config" / L"TdInput.ini";
}

// A kept alternate key the game doesn't have (as after a restart) must be put back at the next reset moment (the
// module switched on), on a key nothing else uses. Uses a file of its own, and the game's ini is written back after.
void TestKeepKeyBindings()
{
    Step& s = Begin("Keep key bindings");
    Probe p(s);
    Module* keep = modules::Find("Keep Key Bindings");
    const auto ini = TdInputIni();
    std::string iniWas;
    const bool hadIni = !ini.empty() && std::filesystem::exists(ini);
    if (hadIni) {
        std::ifstream in(ini, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        iniWas = ss.str();
    }
    if (!keep) {
        p.Problem("the Keep Key Bindings module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = keep->Enabled();
    OnRenderThread([&] { keep->SetEnabled(false); });

    // A key nothing is bound to, and the bindings the game has now
    std::string freeKey;
    nlohmann::json list = nlohmann::json::array();
    OnGameThread([&] {
        auto* pc = game::Controller();
        if (!pc || !pc->PlayerInput) return;
        std::set<std::string> used;
        auto& binds = pc->PlayerInput->Bindings;
        for (std::size_t i = 0; i < binds.Num(); ++i) {
            const auto& b = binds[i];
            used.insert(b.Name.GetName());
            if (b.Control || b.Shift || b.Alt || !b.Command.IsValid()) continue;
            const std::string key = b.Name.GetName(), cmd = b.Command.ToString();
            if (cmd.rfind("GBA_", 0) == 0 && key.rfind("GBA_", 0) != 0 && key.rfind("XboxTypeS_", 0) != 0)
                list.push_back({{"key", key}, {"command", cmd}});
        }
        for (const char* k : {"NumPadNine", "NumPadEight", "NumPadSeven", "Home", "End", "PageUp"})
            if (!used.count(k)) { freeKey = k; break; }
    });
    const auto file = paths::Dir() / "selftest-keybindings.json";
    if (freeKey.empty()) p.Problem("no free key to test with");
    else {
        list.push_back({{"key", freeKey}, {"command", "GBA_Jump"}});
        {
            std::ofstream out(file, std::ios::trunc);
            out << nlohmann::json{{"bindings", list}}.dump(1);
        }
        keep_bindings::SetFile(file);
        OnRenderThread([&] { keep->SetEnabled(true); });     // A reset moment: what is missing comes back
        const auto bound = [&] {
            bool found = false;
            OnGameThread([&] {
                auto* pc = game::Controller();
                if (!pc || !pc->PlayerInput) return;
                auto& binds = pc->PlayerInput->Bindings;
                for (std::size_t i = 0; i < binds.Num() && !found; ++i)
                    found = binds[i].Name.GetName() == freeKey && binds[i].Command.IsValid() &&
                            binds[i].Command.ToString() == "GBA_Jump";
            }, 1.0);
            return found;
        };
        if (!WaitFor(5.0, bound)) p.Problem("the kept binding " + freeKey + " = GBA_Jump was not put back");
        else s.notes.push_back("put back " + freeKey + " = GBA_Jump");

        // Undo: the module off first (it would put it back), then the binding gone and the game's ini as it was
        OnRenderThread([&] { keep->SetEnabled(false); });
        keep_bindings::SetFile({});
        // Taken out of the list, not blanked: the game saves TdInput.ini from memory later (on a level load), and a
        // blanked entry ("NumPadNine", Command=) would end up in the player's file. Removing needs no allocation: the
        // entries after it move down one and the count drops (its command string is left to leak, a few bytes).
        OnGameThread([&] {
            auto* pc = game::Controller();
            if (!pc || !pc->PlayerInput) return;
            auto& binds = pc->PlayerInput->Bindings;
            struct RawArray { Classes::FKeyBind* data; int count; int max; };
            static_assert(sizeof(RawArray) == sizeof(binds), "TArray layout");
            auto* raw = reinterpret_cast<RawArray*>(&binds);
            for (int i = raw->count - 1; i >= 0; --i) {
                if (raw->data[i].Name.GetName() != freeKey) continue;
                std::memmove(&raw->data[i], &raw->data[i + 1], sizeof(Classes::FKeyBind) * (raw->count - i - 1));
                --raw->count;
            }
        });
        if (bound()) p.Problem("could not remove the test binding again");
    }
    if (hadIni) {
        std::ofstream out(ini, std::ios::binary | std::ios::trunc);
        out << iniWas;
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    OnRenderThread([&] { keep->SetEnabled(enabledWas); });
    p.Check(1, false);
    End(s);
}
#endif

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
    auto* labels = dynamic_cast<ctl::Checkbox*>(view->FindSetting("Labels"));     // Drawn meanwhile (on screen)
    const bool labelsWas = labels && labels->Get();
    const bool enabledWas = view->Enabled(), expandedWas = view->expanded, inspectWas = inspect->Get();
    unsigned yawWas = 0, pitchWas = 0;
    OnGameThread([&] { if (auto* pc = game::Controller()) { yawWas = pc->Rotation.Yaw; pitchWas = pc->Rotation.Pitch; } });
    OnRenderThread([&] {
        view->expanded = true;
        inspect->Set(true);
        if (labels) labels->Set(true);
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
        if (labels) labels->Set(labelsWas);
        inspect->Set(inspectWas);
        view->expanded = expandedWas;
        view->SetEnabled(enabledWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// Static geometry in the collision view: the level is read (a slice per tick), real collision comes out of it (not only
// bounding boxes: the mesh and body setup pointers check out), something is drawn near the player, and the game thread
// never pays for it in one go (its worst tick stays small; the build runs on the worker).
void TestCollisionStatic()
{
    Step& s = Begin("Collision view static geometry");
    Probe p(s);
    Module* view = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Collision View") view = &m; });
    auto* on = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Show static geometry")) : nullptr;
    auto* other = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Static: other")) : nullptr;
    if (!view || !on || !other) {
        p.Problem("the Collision View module or its static geometry settings were not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = view->Enabled(), onWas = on->Get(), otherWas = other->Get();
    OnRenderThread([&] {
        on->Set(true);
        other->Set(true);
        if (!view->Enabled()) view->SetEnabled(true);
    });

    staticworld::Info info;
    const bool read = WaitFor(30.0, [&] { info = staticworld::GetInfo(); return !info.map.empty() && !info.scanning && info.meshes > 0; });
    WaitFor(5.0, [&] { info = staticworld::GetInfo(); return info.vertices > 0; });
    WaitTicks(120);                 // The steady state: the level read, the drawing kept up to date
    info = staticworld::GetInfo();
    std::size_t found = 0, drawn = 0;
    std::string per;
    for (int c = 0; c < rstatic::kCategoryCount; ++c) {
        found += info.found[static_cast<std::size_t>(c)];
        drawn += info.drawn[static_cast<std::size_t>(c)];
        per += std::string(per.empty() ? "" : ", ") + std::to_string(info.drawn[static_cast<std::size_t>(c)]) + "/" +
               std::to_string(info.found[static_cast<std::size_t>(c)]) + " " + rstatic::CategoryName(static_cast<rstatic::Category>(c));
    }
    char note[240];
    std::snprintf(note, sizeof note, "%s: %zu meshes with simple collision, %zu placed; drawn/found: %s", info.map.c_str(),
                  info.meshes, found, per.c_str());
    s.notes.push_back(note);
    std::snprintf(note, sizeof note, "%zu k vertices built in %.1f ms (worker); game thread %.3f ms a tick on average, "
                  "slowest %.1f ms%s", info.vertices / 1000, info.buildMs, info.tickAvgMs, info.tickMaxMs,
                  info.budgetHit ? "; vertex limit hit" : "");
    s.notes.push_back(note);
    if (!read) p.Problem("the level was never read (scanning " + std::string(info.scanning ? "still" : "not") + " going)");
    else if (info.meshes == 0) p.Problem("no mesh had simple collision we could read (only bounding boxes): check the "
                                         "StaticMesh / RB_BodySetup offsets");
    if (read && drawn == 0) p.Problem("nothing drawn near the player");
    if (info.tickMaxMs > 4.0) p.Problem("a game thread tick took " + std::to_string(info.tickMaxMs) + " ms");
    if (info.tickAvgMs > 0.5) p.Problem("the game thread spends " + std::to_string(info.tickAvgMs) + " ms a tick on it");

    OnRenderThread([&] {
        on->Set(onWas);
        other->Set(otherWas);
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
    // The developer tools themselves: switching them would close the Test window (or developer mode) under the run
    if (m.Name() == devmode::kModuleName || m.Name() == "Test") return false;
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
const std::set<std::string> kDrawsHud = {"Crosshair", "Show FPS", "Watermark", "Speedometer", "Input Overlay", "Kickglitch Helper",
                                         "Speedrun Timer"};

// What the menu draws with its window closed (the modules' HUDs), a few frames after a change.
unsigned HudVertices()
{
    hooks::DescribeDrawLists(true);
    OnRenderThread([] { gui::visible = false; });
    // Until the count settles: in fast mode a few ticks can pass before the next frame is drawn, and the frame drawn
    // just before a module switched off still holds what it drew (a HUD that changes every frame, like the speedometer's
    // digits, never settles: then the last count)
    WaitTicks(3);
    unsigned last = hooks::MenuVertices();
    for (int i = 0; i < 20; ++i) {
        WaitTicks(3);
        const unsigned now = hooks::MenuVertices();
        if (now == last) break;
        last = now;
    }
    return last;
}

#if MEMENU_GAME
// Mirage step 4: the example mods (examples/mods: hello built by its own CMake project with clang-cl, hello-lua), put in
// selftest-mods by `memenu game-test`, are loaded; their modules are in the menu, run, draw, save and load their
// settings like built-in ones, and keep their own config.
void TestMirageMods()
{
    Step& s = Begin("Mirage mods");
    Probe p(s);
    const auto mods = mirage::mods::List();
    for (const char* id : {"hello", "hello-lua"}) {
        const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == id; });
        if (it == mods.end()) {
            p.Problem(std::string(id) + " was not found (is selftest-mods there? memenu game-test puts it)");
            continue;
        }
        if (!it->loaded) p.Problem(std::string(id) + " was not loaded: " + it->problem);
        else s.notes.push_back(it->id + " " + it->version + " (" + it->kind + "): " + std::to_string(it->modules.size()) + " module(s)");
        for (const auto& w : it->warnings) p.Problem(std::string(id) + ": " + w);
    }

    for (const char* name : {"Hello (native)", "Hello (Lua)"}) {
        Module* m = modules::Find(name);
        if (!m) {
            p.Problem(std::string(name) + " is not in the menu");
            continue;
        }
        if (m->Tab() != "Mods") p.Problem(std::string(name) + " is in the " + m->Tab() + " column, not Mods");
        auto* greeting = dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Greeting"));
        auto* height = dynamic_cast<ctl::Value<float>*>(m->FindSetting("Height"));
        auto* ticks = dynamic_cast<ctl::Value<int>*>(m->FindSetting("Ticks"));
        auto* status = dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Status"));
        if (!greeting || !height || !ticks || !status) {
            p.Problem(std::string(name) + ": its settings are missing or of the wrong type");
            continue;
        }

        // Settings save and load like the menu's own (the config writes each with Save, reads it with Load)
        {
            nlohmann::ordered_json saved;
            const std::string before = greeting->Get();
            greeting->Set("saved greeting");
            greeting->Save(saved["Greeting"]);
            height->Set(0.5f);
            height->Save(saved["Height"]);
            greeting->Set("changed");
            height->Set(0.1f);
            greeting->Load(saved["Greeting"]);
            height->Load(saved["Height"]);
            if (greeting->Get() != "saved greeting" || std::fabs(height->Get() - 0.5f) > 1e-6f)
                p.Problem(std::string(name) + ": settings didn't load back what was saved");
            height->Set(2.0f);
            if (height->Get() > 0.95f) p.Problem(std::string(name) + ": Height isn't clamped to its range");
            greeting->Set(before);
            height->Set(0.3f);
        }

        // On: on_enable, on_tick (Ticks counts) and on_render (it draws); off: on_disable
        const int ticksBefore = ticks->Get();
        OnRenderThread([] { gui::visible = false; });
        WaitTicks(3);
        const unsigned drawnBefore = hooks::MenuVertices();
        OnRenderThread([m] { m->SetEnabled(true); });
        WaitTicks(10);
        const unsigned drawnOn = hooks::MenuVertices();
        if (!m->Enabled()) p.Problem(std::string(name) + " didn't switch on");
        if (status->Get().rfind("on (", 0) != 0) p.Problem(std::string(name) + ": on_enable didn't run (Status '" + status->Get() + "')");
        if (ticks->Get() <= ticksBefore) p.Problem(std::string(name) + ": on_tick didn't run");
        if (drawnOn <= drawnBefore) p.Problem(std::string(name) + ": on_render drew nothing");
        OnRenderThread([m] { m->SetEnabled(false); });
        OnRenderThread([] { gui::visible = true; });
        WaitTicks(3);
        if (status->Get() != "off") p.Problem(std::string(name) + ": on_disable didn't run");
        s.notes.push_back(std::string(name) + ": " + std::to_string(ticks->Get() - ticksBefore) + " ticks, drew " +
                          std::to_string(drawnOn - drawnBefore) + " vertices, " + status->Get());
    }

    // Each mod's own config (config.toml in its folder, written by mirage.config.set / mirage::SetConfig)
    for (const char* id : {"hello", "hello-lua"}) {
        const auto file = paths::Dir() / "selftest-mods" / id / "config.toml";
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        if (text.str().find("times_enabled") == std::string::npos)
            p.Problem(std::string(id) + ": config.toml has no times_enabled");
    }
    p.Check(1, true);
    End(s);
}
#endif

void TestModule(Module& m)
{
    Step& s = Begin("module " + m.Tab() + "/" + m.Name());
    const unsigned n = g_options.ticks;
    Probe p(s);
    const bool drawsHud = kDrawsHud.count(m.Name()) != 0;
    unsigned hudBefore = 0;
    std::string listsBefore;
    // While a HUD is measured, the other HUDs whose text changes as the game runs (Game Status counts ticks, events and
    // health, Collision View the volumes the player is in) are off: they made "still draws after switching off" fail
    // with nothing left of the module's own
    std::vector<Module*> quieted;
    if (drawsHud) {
        std::set<std::string> noisy(kDrawsHud.begin(), kDrawsHud.end());
        noisy.insert({"Game Status", "Info Lines", "Tool Overlay", "Collision View"});
        for (const std::string& name : noisy)
            if (Module* other = modules::Find(name); other && other != &m && other->Enabled()) quieted.push_back(other);
        OnRenderThread([&quieted] { for (Module* q : quieted) q->SetEnabled(false); });
    }
    if (drawsHud) {
        OnRenderThread([&m] { if (m.Enabled()) m.SetEnabled(false); });
        hudBefore = HudVertices();
        listsBefore = hooks::MenuDrawLists();
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
                      std::to_string(hudBefore) + " before it was on; now: " + hooks::MenuDrawLists() +
                      "; before: " + listsBefore + ")");
        OnRenderThread([] { gui::visible = true; });
        OnRenderThread([&quieted] { for (Module* q : quieted) q->SetEnabled(true); });
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

// The feature tests, by name (--only / --skip pick them like modules). `fastTas`: its playbacks don't depend on pacing,
// so in fast mode they run as fast as they go. `optIn`: measurements, only when picked by name.
struct Named { const char* name; void (*run)(); bool fastTas; bool optIn = false; };

const Named* FeatureList()
{
#if MEMENU_GAME
    static const Named tests[] = {
#if MEMENU_FEATURE_TAS
        {"TAS macro", &TestTasMacro, true},
        {"TAS camera keys", &TestTasCamera, true},
        {"TAS go to and step", &TestTasGoto, true},
        {"TAS desync fixing", &TestTasDesync, true},
        {"TAS snapshots", &TestTasSnapshots, true},
        {"TAS workflow", &TestTasWorkflow, false},      // Records and loops in real time
        {"TAS memory survey", &TestTasMemorySurvey, false, true},   // Measurements only: --only
        {"Rendering thread off", &TestRenderingThreadOff, false, true},
#if MEMENU_FEATURE_INPUT_OVERLAY
        {"Input overlay", &TestInputOverlay, true},
#endif
#endif
#if MEMENU_FEATURE_KICKGLITCH_HELPER
        {"Kickglitch helper", &TestKickglitchHelper, false},
#endif
#if MEMENU_FEATURE_AUTO_KICKGLITCH
        {"Auto kickglitch", &TestAutoKickglitch, false},
#endif
#if MEMENU_FEATURE_KILL_BOTS
        {"Kill bots", &TestKillBots, false},
#endif
#if MEMENU_FEATURE_KEY_BINDINGS
        {"Keep key bindings", &TestKeepKeyBindings, false},
#endif
#if MEMENU_FEATURE_COLLISION_VIEW
        {"Collision view inspect", &TestCollisionInspect, false},
        {"Collision view static geometry", &TestCollisionStatic, false},
#endif
#if MEMENU_FEATURE_SPEEDRUN_TIMER
        {"Speedrun timer", &TestSpeedrunTimer, false},
#endif
#if MEMENU_FEATURE_STRANG
        {"Engine state hook", &TestEngineStateHook, false},
#endif
        {"Mirage runtime", &TestMirageRuntime, false},
        {"Mirage layouts", &TestMirageLayouts, false},
        {"Mirage calls", &TestMirageCalls, false},
        {"Mirage mods", &TestMirageMods, false},
        {"Engine script events", &TestEngineScriptEvents, false},
#if MEMENU_FEATURE_FRAME_RATE
        {"Frame rate limit", &TestFrameRate, false},
#endif
        {"Engine console", &TestEngineConsole, false},
        {"Engine death hook", &TestEngineDeath, false},     // Last: it restarts the checkpoint
        {nullptr, nullptr, false},
    };
    return tests;
#else
    static const Named none[] = {{nullptr, nullptr, false}};
    return none;
#endif
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

// The whole test (both ways: the `selftest` file and the Test module). Leaves the menu open when run from the menu.
static void RunTests(bool fromMenu)
{
#if MEMENU_GAME
    static bool listening = false;      // Once: every run would add another callback
    if (!listening) game::OnLevelLoad(&OnLevelLoad);
    listening = true;
#endif
#if MEMENU_FEATURE_TAS
    // Hang watchdog: no frame for 8 s, the game's threads' stacks go to the log (once a hang)
    static std::atomic<bool> watching{true};
    watching = true;                    // Again for every run
    std::thread watchdog([] {
        unsigned last = hooks::EndSceneCalls();
        ULONGLONG since = GetTickCount64();
        bool dumped = false;
        while (watching) {
            Sleep(250);
            const unsigned now = hooks::EndSceneCalls();
            static ULONGLONG loadingSince = 0;
            if (game::IsLoading()) {
                if (!loadingSince) loadingSince = GetTickCount64();
            } else {
                loadingSince = 0;
            }
            if (now != last && !(loadingSince && GetTickCount64() - loadingSince > 10000)) {
                last = now;
                since = GetTickCount64();
                dumped = false;
                continue;
            }
            if (dumped || (GetTickCount64() - since < 8000 && !loadingSince)) continue;
            dumped = true;
            spdlog::warn("Hang: no frame for 8 s, or loading for 10 s (step: {})", g_current);
            for (int round = 0; round < 2; ++round) {
                for (const auto& t : tas::memmap::Threads()) {
                    if (t.startModule != "mirrorsedge.exe" && t.startModule.rfind("physx", 0) != 0) continue;
                    std::string line;
                    for (const auto& w : tas::memmap::StackSample(t.id, 32)) line += w + " ";
                    spdlog::warn("Hang: thread {} ({}) state {} wait {}: {}", t.id, t.startModule, t.state, t.waitReason, line);
                }
                spdlog::default_logger()->flush();
                Sleep(1000);
            }
        }
    });
    // Ends before the test does: the menu unloads after it
    struct StopWatchdog {
        std::thread& t;
        ~StopWatchdog() { watching = false; if (t.joinable()) t.join(); }
    } stopWatchdog{watchdog};
#endif
    const auto saved = SaveAll();
    notify::SetMuted(true);         // Nothing on screen but what the modules draw (the HUD checks measure it)
    if (g_options.fast) SetFrameRateCap(false);

    if (TestFrames()) {
#if MEMENU_GAME
        if (TestGame() && !g_aborted && TestLevel() && !g_aborted &&
            (g_options.keepLevel || Lower(g_options.map) != "none")) {
            TestInputBlocking();
            const Named* tests = FeatureList();
            for (const Named* t = tests; t->name && !g_aborted; ++t) {
                if (!g_options.only.empty() && !g_options.only.count(t->name)) continue;
                if (t->optIn && !g_options.only.count(t->name)) continue;
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
    g_restoring = true;                 // Stopped or not: everything goes back the way it was
    if (g_options.fast) SetFrameRateCap(true);
    notify::SetMuted(false);
    RestoreAll(saved);
    g_restoring = false;
    gui::visible = fromMenu;            // From the menu: the menu stays open on the results
}

void Run()
{
    if (!g_requested) return;
    RunTests(false);
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

// ---- Runs from the menu (developer mode) --------------------------------------------------------------------------------
std::vector<TestInfo> FeatureTests()
{
    std::vector<TestInfo> out;
    for (const Named* t = FeatureList(); t->name; ++t) out.push_back({t->name, t->optIn});
    return out;
}

bool Running() { return g_interactive.load(); }

void Stop()
{
    if (!g_interactive) return;
    g_stop = true;
    spdlog::info("Self-test: stopping...");
}

bool Start(RunOptions o)
{
    if (g_requested || g_interactive.exchange(true)) return false;   // A run (or the file's test) is going
    if (g_worker.joinable()) g_worker.join();                        // The last run's thread: already done

    g_options = Options{};
    g_options.only = std::move(o.only);
    g_options.keepLevel = !o.loadLevel;
    g_options.map = o.map.empty() ? "edge_p" : o.map;
    g_options.checkpoint = std::max(0, o.checkpoint);
    g_options.ticks = std::clamp(o.ticks, 2u, 3600u);
    g_options.fast = o.fast;
    g_options.quit = false;
    g_steps.clear();
    g_current.clear();
    g_aborted = false;
    g_stop = false;
    g_started = GetTickCount64();

    // How many steps to expect, for the progress bar: frames, game, level, input blocking, the picked feature tests and
    // modules, all modules on (full runs), performance
    int planned = 2;
#if MEMENU_GAME
    planned += 2;
    for (const Named* t = FeatureList(); t->name; ++t)
        if (g_options.only.empty() ? !t->optIn : g_options.only.count(t->name) != 0) ++planned;
#endif
    modules::ForEach([&](modules::Tab&, Module& m) { if (Selected(m)) ++planned; });
    if (g_options.only.empty()) ++planned;

    std::vector<logging::Entry> ignored;
    const std::uint64_t firstLog = logging::CopySince(0, ignored);
    {
        std::lock_guard lock(g_publishMutex);
        g_published = Progress{};
        g_published.running = true;
        g_published.run = ++g_runs;
        g_published.planned = planned;
        g_published.firstLogId = firstLog;
    }
    spdlog::info("Self-test: run {} started: {}, {}, {} ticks per module, {}", g_runs,
                 g_options.only.empty() ? "everything" : std::to_string(g_options.only.size()) + " test(s) picked",
                 g_options.keepLevel ? "current level" : "level " + g_options.map, g_options.ticks,
                 g_options.fast ? "fast mode" : "real speed");

    g_worker = std::thread([] {
        config::SetAutoSave(false);     // The test switches settings: never into the player's config (put back after)
        RunTests(true);
        WriteResult(true);              // selftest-result.json, like `memenu game-test`
        int failed = 0;
        for (const Step& st : g_steps) failed += st.ok ? 0 : 1;
        const double seconds = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
        if (g_aborted)
            spdlog::warn("Self-test: run stopped after {} steps ({} failed), {:.1f} s", g_steps.size(), failed, seconds);
        else if (failed)
            spdlog::error("Self-test: run FAILED: {} of {} steps failed, {:.1f} s", failed, g_steps.size(), seconds);
        else
            spdlog::info("Self-test: run PASSED: {} steps, {:.1f} s", g_steps.size(), seconds);
        config::SetAutoSave(true);
        {
            std::lock_guard lock(g_publishMutex);
            g_published.running = false;
            g_published.stopped = g_aborted;
            g_published.seconds = seconds;
        }
        g_interactive = false;
    });
    return true;
}

void Shutdown()
{
    if (g_interactive) g_stop = true;
    if (g_worker.joinable()) g_worker.join();
}

Progress Snapshot()
{
    std::lock_guard lock(g_publishMutex);
    Progress p = g_published;
    if (p.running) p.seconds = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
    return p;
}

} // namespace selftest
