#include "tas/tas.h"
#include "tas/engine.h"

#include "core/notify.h"
#include "game/game.h"
#include "game/levels.h"
#include "gui.h"
#include "paths.h"

#include <Windows.h>
#include <timeapi.h>

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tas {

namespace {

constexpr double kStartTime = 100.0;              // virtual clock at the start of a run (any fixed value works)
constexpr double kArmTimeout = 30.0;              // real seconds to wait for the start level to load
constexpr float kArmHz = 60.f;                    // frames before the first one of a real-time run
constexpr std::size_t kNoSeek = std::numeric_limits<std::size_t>::max();

// ---- shared state ----------------------------------------------------------------------------------------------------------
std::mutex g_mutex;                               // the replay and g_status
Replay g_replay;
Status g_status;

std::mutex g_optMutex;
Options g_options;

std::mutex g_cmdMutex;
std::vector<std::function<void()>> g_commands;

std::atomic<bool> g_active{false};   // a session runs (mirrors s.mode != Idle for other threads)
std::atomic<bool> g_paused{false};
std::atomic<int> g_advance{0};

// ---- game thread state -----------------------------------------------------------------------------------------------------
struct Session {
    Mode mode = Mode::Idle;
    bool armed = false;               // the start level hasn't loaded yet
    bool sawLoad = false;
    double armStart = 0.0;
    std::size_t frame = 0;
    double vtime = kStartTime;
    double lastReal = 0.0;            // real time the previous frame started (pacing)
    std::size_t seekTo = kNoSeek;
    bool renderingOff = false;
    bool endReached = false;
    long long desyncFrame = -1;
    std::size_t syncChecked = 0;

    std::vector<engine::RawInput> pending;              // real input of this frame, recorded at the next one
    std::unordered_map<long long, std::uint16_t> names; // engine FName -> replay name (recording)
    std::vector<int> resolved;                          // replay name -> engine FName index, -1 unknown (playback)
    std::vector<int> held;                              // engine FName indices pressed by injected input
    std::vector<Classes::ULevelStreaming*> streaming;   // levels we set bShouldBlockOnLoad on
    std::unordered_map<long long, bool> hotkeyNames;    // engine FName -> is one of our keys (cache)
    int hotkeyHash = 0;
};
Session s;
Options opt;                          // copy for this frame

void SetMessage(std::string text, bool toast = false)
{
    spdlog::info("TAS: {}", text);
    if (toast) notify::Push("TAS: " + text);
    std::lock_guard lock(g_mutex);
    g_status.message = std::move(text);
}

void Post(std::function<void()> fn)
{
    std::lock_guard lock(g_cmdMutex);
    g_commands.push_back(std::move(fn));
}

void RunCommands()
{
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(g_cmdMutex);
        work.swap(g_commands);
    }
    for (auto& fn : work) fn();
}

float NominalDt(float hz) { return static_cast<float>(1.0 / (hz > 0.f ? hz : kArmHz)); }

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string EngineName(int index, int number)
{
    Classes::FName n(index);
    n.Number = number;
    return n.GetName();
}

// ---- keys ------------------------------------------------------------------------------------------------------------------
// Unreal's name for a virtual key, for the keys a TAS hotkey is likely to be on.
std::string UnrealKeyName(int vk)
{
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    static const char* digits[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    switch (vk) {
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_PRIOR:  return "PageUp";
        case VK_NEXT:   return "PageDown";
        case VK_PAUSE:  return "Pause";
        default:        return {};
    }
}

bool SameName(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

// Keys that control the menu or the session never reach the game while a session runs (they'd be recorded otherwise).
bool IsHotkey(const engine::RawInput& in)
{
    const int hash = opt.keyPause * 31 + opt.keyAdvance * 37 + opt.keyTakeOver * 41 + opt.keyStop * 43;
    if (hash != s.hotkeyHash) {
        s.hotkeyNames.clear();
        s.hotkeyHash = hash;
    }
    const long long key = (static_cast<long long>(in.nameIndex) << 32) | static_cast<unsigned>(in.nameNumber);
    if (auto it = s.hotkeyNames.find(key); it != s.hotkeyNames.end()) return it->second;

    const std::string name = EngineName(in.nameIndex, in.nameNumber);
    bool hit = false;
    for (int vk : {opt.keyPause, opt.keyAdvance, opt.keyTakeOver, opt.keyStop, VK_INSERT, VK_END})
        if (vk && SameName(name, UnrealKeyName(vk))) hit = true;
    s.hotkeyNames[key] = hit;
    return hit;
}

bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// ---- forward declarations ------------------------------------------------------------------------------------------------
void Begin(Mode mode);
void StopNow(const char* why);
void TakeOverNow();

void PollHotkeys()
{
    static bool was[4]{};
    const int keys[4] = {opt.keyPause, opt.keyAdvance, opt.keyTakeOver, opt.keyStop};
    bool edge[4]{};
    for (int i = 0; i < 4; ++i) {
        const bool down = keys[i] && (GetAsyncKeyState(keys[i]) & 0x8000);
        edge[i] = down && !was[i];
        was[i] = down;
    }
    if (s.mode == Mode::Idle || !GameFocused()) return;

    if (edge[0]) g_paused = !g_paused;
    if (edge[1]) {
        if (g_paused) ++g_advance;
        else g_paused = true;
    }
    if (edge[2] && s.mode == Mode::Playing) TakeOverNow();
    if (edge[3]) StopNow("stopped");
}

// ---- input --------------------------------------------------------------------------------------------------------------
bool FilterInput(const engine::RawInput& in)
{
    if (s.mode == Mode::Idle) return false;
    if (!in.axis && in.event == 2) return true;               // OS key repeat: timed by Windows, never reproducible
    if (IsHotkey(in)) return true;
    if (s.mode == Mode::Playing || s.armed) return true;      // the replay owns the input

    // Recording. While the menu is open only releases go through (so nothing stays held).
    const bool release = !in.axis && in.event == 1;
    if (gui::visible && !release) return true;
    s.pending.push_back(in);                                  // recorded and injected at the start of the next frame
    return true;
}

void InjectTracked(const engine::RawInput& in)
{
    engine::Inject(in);
    if (in.axis) return;
    auto it = std::find(s.held.begin(), s.held.end(), in.nameIndex);
    if (in.event == 0 && it == s.held.end()) s.held.push_back(in.nameIndex);
    if (in.event == 1 && it != s.held.end()) s.held.erase(it);
}

// Releases every key the session pressed, so stopping in the middle of a replay doesn't leave the player running.
void ReleaseHeld()
{
    for (const auto& in : s.pending)
        if (!in.axis && in.event == 1) engine::Inject(in);
    s.pending.clear();
    for (int name : s.held) {
        engine::RawInput up;
        up.nameIndex = name;
        up.event = 1;
        engine::Inject(up);
    }
    s.held.clear();
}

void ResetPlayerInput()
{
    if (auto* pc = game::Controller(); pc && pc->PlayerInput) pc->PlayerInput->ResetInput();
}

int Resolve(std::uint16_t name)
{
    if (name >= s.resolved.size()) s.resolved.resize(g_replay.names.size(), -2);
    if (name >= s.resolved.size()) return -1;
    if (s.resolved[name] == -2) {
        const std::string& str = g_replay.Name(name);
        Classes::FName fn(str.c_str());
        const bool found = fn.Index != 0 || SameName(str, "None");
        s.resolved[name] = found ? fn.Index : -1;
        if (!found) spdlog::warn("TAS: key '{}' doesn't exist in this game, its inputs are skipped", str);
    }
    return s.resolved[name];
}

// ---- world -----------------------------------------------------------------------------------------------------------------
Sync Sample()
{
    Sync out;
    if (auto* pawn = game::Pawn()) {
        out.present = 1;
        out.x = pawn->Location.X;
        out.y = pawn->Location.Y;
        out.z = pawn->Location.Z;
        out.vx = pawn->Velocity.X;
        out.vy = pawn->Velocity.Y;
        out.vz = pawn->Velocity.Z;
        out.state = static_cast<std::uint8_t>(pawn->MovementState.GetValue());
    }
    return out;
}

// A streamed sublevel normally loads in the background and appears whenever it is done: a different frame on every
// machine and every run. Blocking makes it appear in the frame it is requested.
void BlockStreaming()
{
    if (!opt.blockStreaming) return;
    auto* world = game::World();
    if (!world) return;
    for (std::size_t i = 0; i < world->StreamingLevels.Num(); ++i) {
        auto* level = world->StreamingLevels[i];
        if (!level || level->bShouldBlockOnLoad) continue;
        level->bShouldBlockOnLoad = 1;
        s.streaming.push_back(level);
    }
}

void RestoreStreaming()
{
    if (auto* world = game::World()) {
        for (std::size_t i = 0; i < world->StreamingLevels.Num(); ++i) {
            auto* level = world->StreamingLevels[i];
            if (level && std::find(s.streaming.begin(), s.streaming.end(), level) != s.streaming.end())
                level->bShouldBlockOnLoad = 0;
        }
    }
    s.streaming.clear();
}

void SetRendering(bool on)
{
    if (s.renderingOff == !on) return;
    s.renderingOff = !on;
    engine::SetWorldRendering(on);
}

bool IssueStart()
{
    const Replay& r = g_replay;
    if (r.start == StartKind::Command) {
        auto* pc = game::Controller();
        if (!pc || r.command.empty()) return false;
        const std::wstring cmd = Widen(r.command);
        pc->ConsoleCommand(Classes::FString(cmd.c_str()), false);
        return true;
    }
    if (r.start == StartKind::Checkpoint) {
        const auto maps = levels::Maps();
        for (std::size_t m = 0; m < maps.size(); ++m) {
            if (maps[m].label != r.map) continue;
            int cp = 0;
            for (std::size_t c = 0; c < maps[m].checkpoints.size(); ++c)
                if (maps[m].checkpoints[c] == r.checkpoint) cp = static_cast<int>(c) + 1;
            if (!r.checkpoint.empty() && cp == 0) return false;
            levels::Load(static_cast<int>(m), cp, levels::Method::Game, false);
            return true;
        }
    }
    return false;
}

// ---- pacing ----------------------------------------------------------------------------------------------------------------
void SleepUntil(double target)
{
    for (;;) {
        const double left = target - engine::RealSeconds();
        if (left <= 0.0) return;
        if (left > 0.002) Sleep(1);
        else YieldProcessor();
    }
}

// Waits so frames of `dt` game seconds come out at `speed` x real time. Doesn't try to catch up more than 100 ms.
void Pace(double dt)
{
    const double now = engine::RealSeconds();
    if (s.seekTo != kNoSeek || opt.speed <= 0.f) {
        s.lastReal = now;
        return;
    }
    const double target = s.lastReal + dt / opt.speed;
    if (now < target) SleepUntil(target);
    s.lastReal = std::max(target, engine::RealSeconds() - 0.1);
}

// Real-time recording: this frame's dt is how long the last one really took (with the frame rate cap).
double MeasureDt()
{
    if (opt.realtimeCap > 0.f) SleepUntil(s.lastReal + 1.0 / opt.realtimeCap);
    const double now = engine::RealSeconds();
    const double dt = std::clamp(now - s.lastReal, 1e-4, 0.1);
    s.lastReal = now;
    return dt;
}

// Blocks the game thread while paused. The picture (and the menu) stand still meanwhile: rendering only happens when
// the game ticks. Hotkeys keep working, and the window keeps answering Windows so it's never "not responding".
bool WaitWhilePaused()
{
    bool waited = false;
    while (s.mode != Mode::Idle && g_paused && g_advance == 0) {
        if (!waited) {
            waited = true;
            std::lock_guard lock(g_mutex);
            g_status.paused = true;
            g_status.frame = s.frame;
        }
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        PollHotkeys();
        RunCommands();
        Sleep(1);
    }
    if (g_advance > 0) --g_advance;
    return waited;
}

// ---- session -------------------------------------------------------------------------------------------------------------------
void Begin(Mode mode)
{
    s.mode = mode;
    s.frame = 0;
    s.vtime = kStartTime;
    s.seekTo = kNoSeek;
    s.endReached = false;
    s.desyncFrame = -1;
    s.syncChecked = 0;
    s.pending.clear();
    s.names.clear();
    s.resolved.clear();
    s.held.clear();
    s.lastReal = engine::RealSeconds();

    g_active = true;
    engine::UseVirtualClock(true);
    engine::SetFrameTime(s.vtime, NominalDt(g_replay.fixedHz));

    // Look the replay's key names up now: the engine's name table is long, and frame 0 shouldn't wait for it.
    if (mode == Mode::Playing) {
        std::lock_guard lock(g_mutex);
        for (std::size_t i = 0; i < g_replay.names.size(); ++i) Resolve(static_cast<std::uint16_t>(i));
    }
    timeBeginPeriod(1);
    game::SetInputBlockedSuspended(true);

    {
        std::lock_guard lock(g_mutex);
        g_status.verified = false;
        g_status.desyncFrame = -1;
        g_status.syncChecked = 0;
    }

    if (g_replay.start == StartKind::Now) {
        s.armed = false;
        engine::SeedRand(g_replay.seed);
        ResetPlayerInput();
        SetMessage(mode == Mode::Recording ? "recording" : "playing");
        return;
    }

    s.armed = true;
    s.sawLoad = false;
    s.armStart = engine::RealSeconds();
    if (!IssueStart()) {
        StopNow("couldn't start: the start level/command is not available");
        return;
    }
    SetMessage("loading the start level");
}

void StopNow(const char* why)
{
    if (s.mode == Mode::Idle) return;
    ReleaseHeld();
    if (s.mode == Mode::Recording) {
        std::lock_guard lock(g_mutex);
        g_replay.syncValidUntil = static_cast<std::uint32_t>(g_replay.frames.size());
    }
    s.mode = Mode::Idle;
    s.armed = false;
    s.seekTo = kNoSeek;
    g_active = false;
    g_paused = false;
    g_advance = 0;

    SetRendering(true);
    RestoreStreaming();
    engine::UseVirtualClock(false);
    timeEndPeriod(1);
    game::SetInputBlockedSuspended(false);
    SetMessage(why, true);
}

void TakeOverNow()
{
    if (s.mode != Mode::Playing || s.armed) return;
    {
        std::lock_guard lock(g_mutex);
        if (s.frame < g_replay.frames.size()) {
            g_replay.Truncate(s.frame);
            ++g_replay.rerecords;
        }
        g_replay.syncValidUntil = static_cast<std::uint32_t>(g_replay.frames.size());
    }
    s.mode = Mode::Recording;
    s.seekTo = kNoSeek;
    s.pending.clear();
    s.names.clear();
    s.lastReal = engine::RealSeconds();
    SetRendering(true);
    SetMessage("recording from frame " + std::to_string(s.frame), true);
}

// The player's state against the recording. Frames past the checked range (edited) get their sync data refreshed instead,
// as long as everything before them matched.
void CheckSync(const Sync& now)
{
    Replay& r = g_replay;
    if (r.start == StartKind::Now) return;
    if (r.sync.size() != r.frames.size()) r.sync.resize(r.frames.size());

    if (s.frame < r.syncValidUntil) {
        if (r.sync[s.frame] == now) {
            ++s.syncChecked;
        } else if (s.desyncFrame < 0) {
            s.desyncFrame = static_cast<long long>(s.frame);
            const Sync& want = r.sync[s.frame];
            spdlog::warn("TAS: desync at frame {}: at ({:.3f}, {:.3f}, {:.3f}) state {}, recorded ({:.3f}, {:.3f}, {:.3f}) "
                         "state {}", s.frame, now.x, now.y, now.z, now.state, want.x, want.y, want.z, want.state);
            g_status.message = "desync at frame " + std::to_string(s.frame);
            notify::Push("TAS: desync at frame " + std::to_string(s.frame));
            if (opt.pauseOnDesync) g_paused = true;
        }
    } else if (s.frame == r.syncValidUntil && s.desyncFrame < 0) {
        r.sync[s.frame] = now;
        ++r.syncValidUntil;
    }
}

void PlaybackEnded()
{
    s.endReached = true;
    const Replay& r = g_replay;
    {
        std::lock_guard lock(g_mutex);
        const std::size_t checkable = std::min<std::size_t>(r.syncValidUntil, r.frames.size());
        g_status.verified = r.start != StartKind::Now && s.desyncFrame < 0 && checkable == r.frames.size() &&
                            s.syncChecked == checkable && checkable > 0;
    }
    switch (opt.atEnd) {
        case AtEnd::Record: TakeOverNow(); break;
        case AtEnd::Pause:  g_paused = true; SetMessage("end of the replay (paused)"); break;
        case AtEnd::Stop:   StopNow("end of the replay"); break;
    }
}

void PublishStatus()
{
    std::lock_guard lock(g_mutex);
    g_status.mode = s.mode;
    g_status.armed = s.armed;
    g_status.paused = g_paused;
    g_status.seeking = s.seekTo != kNoSeek;
    g_status.frame = s.frame;
    g_status.frames = g_replay.frames.size();
    g_status.time = s.vtime - kStartTime;
    g_status.desyncFrame = s.desyncFrame;
    g_status.syncChecked = s.syncChecked;
}

// Called by the engine's frame timing on the game thread, once per frame, before the engine ticks.
bool OnFrame()
{
    {
        std::lock_guard lock(g_optMutex);
        opt = g_options;
    }
    RunCommands();
    PollHotkeys();
    if (s.mode == Mode::Idle) return false;

    // Arm: wait for the start level, running frames at the nominal step (they are the same in every run too).
    if (s.armed) {
        if (s.sawLoad && !game::IsLoading() && game::Pawn()) {
            s.armed = false;
            s.vtime = kStartTime;      // frame 0 starts at the same clock in every run
            s.lastReal = engine::RealSeconds() - NominalDt(g_replay.fixedHz);   // the load isn't part of frame 0
            SetMessage(s.mode == Mode::Recording ? "recording" : "playing");
        } else if (!s.sawLoad && engine::RealSeconds() - s.armStart > kArmTimeout) {
            StopNow("the start level never loaded");
            return false;
        } else {
            const double dt = NominalDt(g_replay.fixedHz);
            s.vtime += dt;
            engine::SetFrameTime(s.vtime, dt);
            PublishStatus();
            return true;
        }
    }

    if (s.seekTo != kNoSeek && s.frame >= s.seekTo) {
        s.seekTo = kNoSeek;
        g_paused = true;
        SetRendering(true);
    }
    if (WaitWhilePaused()) s.lastReal = engine::RealSeconds();
    if (s.mode == Mode::Idle) return false;

    if (s.mode == Mode::Playing && s.frame >= g_replay.frames.size()) {
        if (!s.endReached) PlaybackEnded();         // stops, pauses, or switches to recording
        if (s.mode == Mode::Playing) {
            // Paused at the end: wait there. Resuming or stepping ends the playback, taking over keeps recording.
            PublishStatus();
            WaitWhilePaused();
            if (s.mode == Mode::Playing) StopNow("end of the replay");
            s.lastReal = engine::RealSeconds();
        }
        if (s.mode != Mode::Recording) return false;
    }

    // This frame's time step.
    double dt;
    if (s.mode == Mode::Playing) {
        dt = g_replay.frames[s.frame].dt;
        Pace(dt);
    } else if (g_replay.fixedHz > 0.f) {
        dt = NominalDt(g_replay.fixedHz);
        Pace(dt);
    } else {
        dt = static_cast<float>(MeasureDt());   // stored as float: use exactly what playback will use
    }
    s.vtime += dt;
    engine::SetFrameTime(s.vtime, dt);

    BlockStreaming();
    const Sync now = Sample();
    {
        std::lock_guard lock(g_mutex);
        Replay& r = g_replay;
        if (s.mode == Mode::Playing) {
            CheckSync(now);
            const Frame& f = r.frames[s.frame];
            for (std::uint32_t i = 0; i < f.count; ++i) {
                const Event& e = r.events[f.first + i];
                const int name = Resolve(e.name);
                if (name < 0) continue;
                engine::RawInput in;
                in.nameIndex = name;
                in.axis = e.kind == EventKind::Axis;
                in.event = in.axis ? 0 : static_cast<int>(e.kind);
                in.value = e.value;
                in.value2 = e.value2;
                in.gamepad = e.gamepad != 0;
                InjectTracked(in);
            }
        } else {
            r.BeginFrame(static_cast<float>(dt));
            r.sync.resize(r.frames.size() - 1);
            r.sync.push_back(now);
            r.syncValidUntil = static_cast<std::uint32_t>(r.frames.size());
            for (const auto& in : s.pending) {
                const long long key = (static_cast<long long>(in.nameIndex) << 32) | static_cast<unsigned>(in.nameNumber);
                auto it = s.names.find(key);
                if (it == s.names.end()) it = s.names.emplace(key, r.NameIndex(EngineName(in.nameIndex, in.nameNumber))).first;
                Event e;
                e.name = it->second;
                e.kind = in.axis ? EventKind::Axis : static_cast<EventKind>(in.event);
                e.gamepad = in.gamepad ? 1 : 0;
                e.value = in.value;
                e.value2 = in.value2;
                r.AddEvent(e);
                InjectTracked(in);
            }
            s.pending.clear();
        }
    }
    ++s.frame;
    PublishStatus();
    return true;
}

void OnLevelLoad(bool finished)
{
    if (finished) return;
    s.streaming.clear();                   // those levels are gone
    if (s.mode != Mode::Idle && s.armed) {
        // The start level is loading: from here on every run does the same, so this is where the run's randomness
        // and clock begin.
        engine::SeedRand(g_replay.seed);
        s.vtime = kStartTime;
        s.sawLoad = true;
    }
}

} // namespace

// ---- public ----------------------------------------------------------------------------------------------------------------
bool Init()
{
    if (!engine::Init()) return false;
    engine::SetFrameHandler(&OnFrame);
    engine::SetInputFilter(&FilterInput);
    game::OnLevelLoad(&OnLevelLoad);
    std::lock_guard lock(g_mutex);
    g_status.available = true;
    return true;
}

bool Available() { return engine::Available(); }

void SetOptions(const Options& options)
{
    std::lock_guard lock(g_optMutex);
    g_options = options;
}

Status GetStatus()
{
    std::lock_guard lock(g_mutex);
    Status st = g_status;
    st.frames = g_replay.frames.size();
    if (st.mode == Mode::Idle) st.paused = false;
    return st;
}

void Record(StartKind start, const std::string& command, const std::string& map, const std::string& checkpoint)
{
    Post([=] {
        StopNow("stopped");
        Options o;
        {
            std::lock_guard lock(g_optMutex);
            o = g_options;
        }
        {
            std::lock_guard lock(g_mutex);
            g_replay.Clear();
            g_replay.start = start;
            g_replay.command = command;
            g_replay.map = map;
            g_replay.checkpoint = checkpoint;
            g_replay.fixedHz = o.recordHz;
            g_replay.gameStamp = engine::GameStamp();
            g_replay.created = static_cast<std::uint64_t>(std::time(nullptr));
        }
        Begin(Mode::Recording);
    });
}

void Play()
{
    Post([] {
        StopNow("stopped");
        bool empty;
        {
            std::lock_guard lock(g_mutex);
            empty = g_replay.frames.empty();
        }
        if (empty) {
            SetMessage("nothing to play", true);
            return;
        }
        Begin(Mode::Playing);
    });
}

void Stop()
{
    // Also lets a paused game thread go (it is waiting in WaitWhilePaused, which runs commands).
    Post([] { StopNow("stopped"); });
}

void TogglePause()
{
    if (g_active) g_paused = !g_paused;
}

void Advance(int frames)
{
    if (!g_active) return;
    if (g_paused) g_advance += frames;
    else g_paused = true;
}

void TakeOver() { Post([] { TakeOverNow(); }); }

void Goto(std::size_t frame)
{
    Post([frame]() mutable {
        StartKind start;
        std::size_t count;
        {
            std::lock_guard lock(g_mutex);
            start = g_replay.start;
            count = g_replay.frames.size();
        }
        if (count == 0) return;
        frame = std::min(frame, count);

        if (s.mode != Mode::Playing || s.armed || frame < s.frame) {
            if (start == StartKind::Now) {
                SetMessage("a macro (start: now) can't be rewound", true);
                return;
            }
            StopNow("seeking");
            Begin(Mode::Playing);
            if (s.mode == Mode::Idle) return;
        }
        s.seekTo = frame;
        g_paused = false;
        g_advance = 0;
        if (opt.seekWithoutRendering) SetRendering(false);
    });
}

void WithReplay(const std::function<void(Replay&)>& fn)
{
    std::lock_guard lock(g_mutex);
    fn(g_replay);
}

bool LoadFile(const std::filesystem::path& path, std::string* error)
{
    Replay r;
    if (!r.Load(path, error)) return false;
    if (r.gameStamp && r.gameStamp != engine::GameStamp())
        SetMessage("warning: made with a different game version, it will likely desync", true);
    // Not while a session uses the replay: stop it first, then swap.
    Post([r = std::move(r)]() mutable {
        StopNow("stopped");
        {
            std::lock_guard lock(g_mutex);
            g_replay = std::move(r);
        }
        s.resolved.clear();
    });
    return true;
}

bool SaveFile(const std::filesystem::path& path, std::string* error)
{
    std::lock_guard lock(g_mutex);
    g_replay.botName = "MirrorsEdgeMenu";
    g_replay.botVersion = "1.0";
    if (!g_replay.gameStamp) g_replay.gameStamp = engine::GameStamp();
    return g_replay.Save(path, error);
}

std::filesystem::path Folder() { return paths::Dir() / "replays"; }

} // namespace tas
