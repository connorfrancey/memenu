#include "tas/tas.h"
#include "tas/engine.h"
#include "tas/input_monitor.h"
#include "tas/examples.h"
#include "tas/timeline.h"

#include "core/notify.h"
#include "game/game.h"
#include "game/levels.h"
#include "gui.h"
#include "hooks.h"
#include "paths.h"

#include <Windows.h>
#include <timeapi.h>

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <cstdlib>
#include <map>
#include <set>
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

// ---- the game's settings a recording depends on ----------------------------------------------------------------------------
// Saved with every recording (Replay::extensions, "env.*") and matched on playback (Options::matchRecording): the same
// mouse movement turns the view by the sensitivity, and a recorded key does whatever it is bound to.
struct Env {
    bool valid = false;               // there was a player to read them from
    float sensitivity = 0.f;
    bool invertMouse = false;
    std::map<std::string, std::string> binds;   // key -> the command it is bound to (without Ctrl / Shift / Alt)
};

std::mutex g_envMutex;
Env g_envShared;                      // the game's, a moment ago (Preflight reads it on any thread)

constexpr const char* kEnv = "env.";

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
    bool recordAfterSeek = false;     // a rewind: switch to recording when the seek arrives
    bool deathSeen = false;           // the player's death was handled already (it lasts many frames)

    std::vector<engine::RawInput> pending;              // real input of this frame, recorded at the next one
    std::unordered_map<long long, std::uint16_t> names; // engine FName -> replay name (recording)
    std::vector<int> resolved;                          // replay name -> engine FName index, -1 unknown (playback)
    std::vector<int> held;                              // engine FName indices pressed by injected input
    std::vector<Classes::ULevelStreaming*> streaming;   // levels we set bShouldBlockOnLoad on
    std::unordered_map<long long, bool> hotkeyNames;    // engine FName -> is one of our keys (cache)
    int hotkeyHash = 0;

    // Desync prevention and fixing
    bool blockStreaming = true;       // this session's streaming setting (the recording's, when matching it)
    std::size_t corrections = 0;      // OnDesync::Correct
    std::string desyncInfo;
    struct SavedInput { bool valid = false; float sensitivity = 0.f; bool invert = false; } savedInput;
    Env recordEnv;                    // the settings when this recording started
    bool recordStreaming = true;
    std::string recordMods;
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

std::string Ext(const Replay& r, const std::string& key)
{
    const auto it = r.extensions.find(kEnv + key);
    return it == r.extensions.end() ? std::string() : it->second;
}
bool HasExt(const Replay& r, const std::string& key) { return r.extensions.count(kEnv + key) != 0; }

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

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
    const int hash = opt.keyPause * 31 + opt.keyAdvance * 37 + opt.keyTakeOver * 41 + opt.keyStop * 43 + opt.keyRewind * 47 +
                     opt.keyRestart * 53 + opt.keyRecord * 59 + opt.keyPlay * 61 + opt.keyBookmark * 67;
    if (hash != s.hotkeyHash) {
        s.hotkeyNames.clear();
        s.hotkeyHash = hash;
    }
    const long long key = (static_cast<long long>(in.nameIndex) << 32) | static_cast<unsigned>(in.nameNumber);
    if (auto it = s.hotkeyNames.find(key); it != s.hotkeyNames.end()) return it->second;

    const std::string name = EngineName(in.nameIndex, in.nameNumber);
    bool hit = false;
    for (int vk : {opt.keyPause, opt.keyAdvance, opt.keyTakeOver, opt.keyStop, opt.keyRewind, opt.keyRestart, opt.keyRecord,
                   opt.keyPlay, opt.keyBookmark, VK_INSERT, VK_END})
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
void RewindNow(const char* why);
void RestartNow();
void PublishStatus();

void RecordNow(StartKind start, const std::string& command, const std::string& map, const std::string& checkpoint,
               float hz);
void Begin(Mode mode);

void PollHotkeys()
{
    constexpr int kKeys = 9;
    static bool was[kKeys]{};
    const int keys[kKeys] = {opt.keyPause, opt.keyAdvance, opt.keyTakeOver, opt.keyStop, opt.keyRewind, opt.keyRestart,
                             opt.keyRecord, opt.keyPlay, opt.keyBookmark};
    bool edge[kKeys]{};
    for (int i = 0; i < kKeys; ++i) {
        const bool down = keys[i] && (GetAsyncKeyState(keys[i]) & 0x8000);
        edge[i] = down && !was[i];
        was[i] = down;
    }
    if (!GameFocused()) return;
    if (s.mode == Mode::Idle) {
        // Starting needs the menu closed: with it open, keys are typed into it
        if (gui::visible) return;
        if (edge[6]) RecordNow(opt.recordStart, opt.recordCommand, opt.recordMap, opt.recordCheckpoint, opt.recordHz);
        if (edge[7]) {
            bool empty;
            {
                std::lock_guard lock(g_mutex);
                empty = g_replay.frames.empty();
            }
            if (empty) SetMessage("nothing to play", true);
            else Begin(Mode::Playing);
        }
        return;
    }

    if (edge[0]) g_paused = !g_paused;
    if (edge[1]) {
        if (g_paused) ++g_advance;
        else g_paused = true;
    }
    if (edge[2] && s.mode == Mode::Playing) TakeOverNow();
    if (edge[3]) StopNow("stopped");
    if (edge[4] && s.mode == Mode::Recording && !s.armed) RewindNow("rewound");
    if (edge[5]) RestartNow();
    if (edge[8] && !s.armed) {
        std::lock_guard lock(g_mutex);
        const auto frame = static_cast<std::uint32_t>(std::min(s.frame, g_replay.frames.empty() ? 0 : g_replay.frames.size() - 1));
        g_replay.SetMarker(frame, "Bookmark " + std::to_string(g_replay.markers.size() + 1));
        notify::Push("TAS: bookmark at frame " + std::to_string(frame));
    }
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

// ---- camera ----------------------------------------------------------------------------------------------------------------
// Unreal rotations: 65536 units to a turn. Yaw keeps counting past a turn; pitch is wrapped to 16 bits.
constexpr double kRotUnits = 65536.0 / 360.0;

void ApplyCamera(float yawDeg, float pitchDeg)
{
    auto* pc = game::Controller();
    if (!pc) return;
    const auto yaw = static_cast<std::int32_t>(std::lround(yawDeg * kRotUnits));
    const auto pitch = static_cast<std::int32_t>(std::lround(std::clamp(pitchDeg, -89.f, 89.f) * kRotUnits));
    pc->Rotation.Yaw = static_cast<unsigned>(yaw);
    pc->Rotation.Pitch = static_cast<unsigned>(pitch) & 0xFFFFu;
}

bool ReadCamera(float& yawDeg, float& pitchDeg)
{
    auto* pc = game::Controller();
    if (!pc) return false;
    int pitch = static_cast<int>(pc->Rotation.Pitch & 0xFFFFu);
    if (pitch > 32768) pitch -= 65536;
    yawDeg = static_cast<float>(static_cast<std::int32_t>(pc->Rotation.Yaw) / kRotUnits);
    pitchDeg = static_cast<float>(pitch / kRotUnits);
    return true;
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

// The game's input settings and key bindings (game thread).
Env ReadEnv()
{
    Env e;
    auto* pc = game::Controller();
    if (!pc || !pc->PlayerInput) return e;
    auto* in = pc->PlayerInput;
    e.valid = true;
    e.sensitivity = in->MouseSensitivity;
    e.invertMouse = in->bInvertMouse != 0;
    for (std::size_t i = 0; i < in->Bindings.Num(); ++i) {
        const auto& b = in->Bindings[i];
        if (b.Control || b.Shift || b.Alt || !b.Command.IsValid()) continue;
        e.binds.emplace(b.Name.GetName(), b.Command.ToString());     // the first binding of a key is the one used
    }
    return e;
}

void PublishEnv()
{
    Env e = ReadEnv();
    if (!e.valid) return;
    std::lock_guard lock(g_envMutex);
    g_envShared = std::move(e);
}

// Saves what this recording depends on, for the keys it uses. Values already there are kept: a recording continued
// later (take over, practice mode) keeps the settings it was started with.
void WriteEnv(Replay& r, const Env& e, bool blockStreaming, const std::string& mods)
{
    const auto put = [&](const std::string& key, std::string value) { r.extensions.emplace(kEnv + key, std::move(value)); };
    put("blockStreaming", blockStreaming ? "1" : "0");
    put("mods", mods);
    if (!e.valid) return;
    char sens[32];
    std::snprintf(sens, sizeof sens, "%.4f", e.sensitivity);
    put("sensitivity", sens);
    put("invertMouse", e.invertMouse ? "1" : "0");
    for (const std::string& name : r.names)
        if (const auto it = e.binds.find(name); it != e.binds.end()) put("bind." + name, it->second);
}

// The key bound to `command` now, preferring one of the same kind (gamepad or not) as `like`. Empty if none.
std::string KeyFor(const Env& e, const std::string& command, const std::string& like)
{
    const bool pad = like.rfind("XboxTypeS_", 0) == 0;
    std::string other;
    for (const auto& [key, cmd] : e.binds) {
        if (cmd != command || key.rfind("GBA_", 0) == 0) continue;     // GBA_ names are aliases, not keys
        if ((key.rfind("XboxTypeS_", 0) == 0) == pad) return key;
        if (other.empty()) other = key;
    }
    return other;
}

// Playback, when frame 0 starts: the recording's mouse settings for this session, and every key whose binding changed
// played as the key that does the recorded thing now. Undone by StopNow.
void ApplyMatch()
{
    if (s.mode != Mode::Playing || !opt.matchRecording) return;
    const Env now = ReadEnv();
    if (!now.valid) return;
    std::lock_guard lock(g_mutex);
    const Replay& r = g_replay;

    auto* pc = game::Controller();
    if (pc && pc->PlayerInput && HasExt(r, "sensitivity")) {
        const float sens = std::strtof(Ext(r, "sensitivity").c_str(), nullptr);
        const bool invert = Ext(r, "invertMouse") == "1";
        if (sens != now.sensitivity || invert != now.invertMouse) {
            if (!s.savedInput.valid) s.savedInput = {true, now.sensitivity, now.invertMouse};
            pc->PlayerInput->MouseSensitivity = sens;
            pc->PlayerInput->bInvertMouse = invert ? 1 : 0;
            spdlog::info("TAS: mouse sensitivity {} (invert {}) as recorded, yours is {} (invert {})", sens, invert,
                         now.sensitivity, now.invertMouse);
        }
    }

    for (std::size_t i = 0; i < r.names.size(); ++i) {
        const std::string& name = r.names[i];
        if (!HasExt(r, "bind." + name)) continue;
        const std::string recorded = Ext(r, "bind." + name);
        const auto cur = now.binds.find(name);
        if (cur != now.binds.end() && cur->second == recorded) continue;
        const std::string key = KeyFor(now, recorded, name);
        if (key.empty()) {
            spdlog::warn("TAS: '{}' was bound to '{}' when recorded; no key is now, it plays as it is", name, recorded);
            continue;
        }
        Classes::FName fn(key.c_str());
        if (i >= s.resolved.size()) s.resolved.resize(r.names.size(), -2);
        s.resolved[i] = fn.Index;
        spdlog::info("TAS: '{}' ({}) plays as '{}', which is bound to that now", name, recorded, key);
    }
}

void RestoreInput()
{
    if (!s.savedInput.valid) return;
    if (auto* pc = game::Controller(); pc && pc->PlayerInput) {
        pc->PlayerInput->MouseSensitivity = s.savedInput.sensitivity;
        pc->PlayerInput->bInvertMouse = s.savedInput.invert ? 1 : 0;
    }
    s.savedInput = {};
}

// A streamed sublevel normally loads in the background and appears whenever it is done: a different frame on every
// machine and every run. Blocking makes it appear in the frame it is requested.
void BlockStreaming()
{
    if (!s.blockStreaming) return;
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

// The game thread's window messages, handled the way the game's own loop does (the menu's PeekMessage hook sees them
// first, so it gets its mouse and keyboard). Input meant for the game goes through the input filter as usual: while
// recording it is held back to the next frame.
void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));   // for the game's own loop, once this frame goes on
            StopNow("the game is closing");
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// The game draws nothing while its thread waits here, so the menu is drawn over the last frame instead and stays
// usable (Resume, Stop, Restart, files...). On the real clock: ImGui times its frames with the performance counter.
void DrawPaused()
{
    // An exception from here would unwind through the engine's frame and end the game: if drawing ever fails, the
    // session stays paused without the menu on screen (hotkeys still work) for the rest of the game.
    static bool broken = false;
    if (broken) return;
    engine::SuspendVirtualClock(true);
    try {
        hooks::DrawWhileStalled();
    } catch (const std::exception& e) {
        broken = true;
        spdlog::error("TAS: drawing the menu while paused failed ({}): not drawn while paused from now on", e.what());
    } catch (...) {
        broken = true;
        spdlog::error("TAS: drawing the menu while paused failed: not drawn while paused from now on");
    }
    engine::SuspendVirtualClock(false);
}

// Blocks the game thread while paused. The game stands still meanwhile; the menu keeps working (DrawPaused), so do the
// hotkeys, and the window keeps answering Windows so it's never "not responding".
bool WaitWhilePaused()
{
    bool waited = false;
    for (unsigned spin = 0; s.mode != Mode::Idle && g_paused && g_advance == 0; ++spin) {
        if (!waited) {
            waited = true;
            std::lock_guard lock(g_mutex);
            g_status.paused = true;
            g_status.frame = s.frame;
        }
        PumpMessages();
        PollHotkeys();
        RunCommands();
        if (s.mode != Mode::Idle) PublishStatus();      // a Record / Play / Restart from the menu shows right away
        if (spin % 16 == 15) DrawPaused();      // about 60 times a second (Sleep(1) with timeBeginPeriod(1))
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
    s.deathSeen = false;
    s.pending.clear();
    s.names.clear();
    s.resolved.clear();
    s.held.clear();
    s.lastReal = engine::RealSeconds();
    s.corrections = 0;
    s.desyncInfo.clear();
    s.blockStreaming = opt.blockStreaming;
    if (mode == Mode::Playing && opt.matchRecording) {
        std::lock_guard lock(g_mutex);
        if (HasExt(g_replay, "blockStreaming")) s.blockStreaming = Ext(g_replay, "blockStreaming") == "1";
    }

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
    hooks::CaptureFrames(true);         // the picture the menu is drawn over while paused

    {
        std::lock_guard lock(g_mutex);
        g_status.verified = false;
        g_status.desyncFrame = -1;
        g_status.desyncInfo.clear();
        g_status.corrections = 0;
        g_status.syncChecked = 0;
    }

    if (g_replay.start == StartKind::Now) {
        s.armed = false;
        engine::SeedRand(g_replay.seed);
        ResetPlayerInput();
        ApplyMatch();
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

// What Unsaved() compares: the inputs (and camera keys), and the bookmarks. Under g_mutex.
std::uint64_t ContentHash(const Replay& r)
{
    std::uint64_t h = r.InputHash() ^ (static_cast<std::uint64_t>(r.frames.size()) << 1);
    for (const Marker& m : r.markers) {
        h = (h ^ m.frame) * 0x100000001B3ull;
        for (char c : m.name) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
    }
    return h;
}
std::uint64_t g_savedHash = ContentHash(Replay{});      // Under g_mutex: the content as last saved / loaded

// autosave/<date-time>.merp, and the oldest ones beyond kAutosaves deleted.
void Autosave(Replay r)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = AutosaveFolder();
    fs::create_directories(dir, ec);
    char name[48];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(name, sizeof name, "%Y-%m-%d %H-%M-%S", &tm);
    r.botName = "MirrorsEdgeMenu";
    r.botVersion = "1.0";
    if (r.description.empty()) r.description = std::string("autosave ") + name;
    std::string error;
    if (!r.Save(dir / (std::string(name) + kFileExtension), &error)) {
        spdlog::warn("TAS: autosave failed: {}", error);
        return;
    }
    std::vector<fs::directory_entry> saves;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == kFileExtension) saves.push_back(e);
    std::sort(saves.begin(), saves.end(), [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
    for (std::size_t i = kAutosaves; i < saves.size(); ++i) fs::remove(saves[i].path(), ec);   // Names sort by date
}

void StopNow(const char* why)
{
    if (s.mode == Mode::Idle) return;
    ReleaseHeld();
    if (s.mode == Mode::Recording) {
        const Env env = s.recordEnv.valid ? s.recordEnv : ReadEnv();
        Replay copy;
        {
            std::lock_guard lock(g_mutex);
            g_replay.syncValidUntil = static_cast<std::uint32_t>(g_replay.frames.size());
            WriteEnv(g_replay, env, s.recordEnv.valid ? s.recordStreaming : s.blockStreaming,
                     s.recordEnv.valid ? s.recordMods : opt.gameplayMods);
            if (g_replay.frames.size() >= 30) copy = g_replay;     // Half a second or more: worth keeping
        }
        if (!copy.frames.empty()) Autosave(copy);
    }
    RestoreInput();
    s.recordEnv = {};
    s.mode = Mode::Idle;
    s.armed = false;
    s.seekTo = kNoSeek;
    s.recordAfterSeek = false;
    g_active = false;
    g_paused = false;
    g_advance = 0;

    SetRendering(true);
    RestoreStreaming();
    engine::UseVirtualClock(false);
    timeEndPeriod(1);
    game::SetInputBlockedSuspended(false);
    hooks::CaptureFrames(false);
    PublishStatus();        // OnFrame returns early from now on: without this the menu would show the old mode forever
    SetMessage(why, true);
}

// A new recording (the replay is replaced) with this start.
void RecordNow(StartKind start, const std::string& command, const std::string& map, const std::string& checkpoint,
               float hz)
{
    StopNow("stopped");
    {
        std::lock_guard lock(g_mutex);
        g_replay.Clear();
        g_replay.start = start;
        g_replay.command = command;
        g_replay.map = map;
        g_replay.checkpoint = checkpoint;
        g_replay.fixedHz = hz;
        g_replay.gameStamp = engine::GameStamp();
        g_replay.created = static_cast<std::uint64_t>(std::time(nullptr));
    }
    s.resolved.clear();
    s.recordEnv = ReadEnv();            // saved with the recording when it stops
    s.recordStreaming = opt.blockStreaming;
    s.recordMods = opt.gameplayMods;
    Begin(Mode::Recording);
}

// Recording: throw the take away and record again from the same start (a level start loads the level again).
// Playing: play again from the start.
void RestartNow()
{
    if (s.mode == Mode::Recording) {
        StartKind start;
        std::string command, map, checkpoint;
        float hz;
        {
            std::lock_guard lock(g_mutex);
            start = g_replay.start;
            command = g_replay.command;
            map = g_replay.map;
            checkpoint = g_replay.checkpoint;
            hz = g_replay.fixedHz;
        }
        RecordNow(start, command, map, checkpoint, hz);
        if (s.mode != Mode::Idle) SetMessage("recording restarted", true);
    } else if (s.mode == Mode::Playing) {
        StopNow("restarting");
        Begin(Mode::Playing);
    }
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

// The frame `seconds` of game time before the current one (0 at most).
std::size_t FramesBack(double seconds)
{
    std::lock_guard lock(g_mutex);
    std::size_t f = std::min(s.frame, g_replay.frames.size());
    double t = 0.0;
    while (f > 0 && t < seconds) t += g_replay.frames[--f].dt;
    return f;
}

// Practice mode: drop the last seconds of the recording, replay the rest (as fast as possible, checked against the
// recording) and continue recording from there, paused so the player can get ready.
void RewindNow(const char* why)
{
    if (s.mode != Mode::Recording || s.armed) return;
    if (g_replay.start == StartKind::Now) {
        SetMessage("a macro that starts now can't rewind (record from a level start)", true);
        return;
    }
    const std::size_t target = FramesBack(opt.rewindSeconds);
    {
        std::lock_guard lock(g_mutex);
        g_replay.Truncate(target);
        ++g_replay.rerecords;
    }
    StopNow(why);
    if (target == 0) {
        Begin(Mode::Recording);                  // nothing left: a fresh attempt from the start level
        return;
    }
    Begin(Mode::Playing);
    if (s.mode == Mode::Idle) return;
    s.seekTo = target;
    s.recordAfterSeek = true;
    g_paused = false;
    g_advance = 0;
    if (opt.seekWithoutRendering) SetRendering(false);
}

// Recording only: reacts once to the player dying (Options::onDeath).
void CheckDeath()
{
    if (s.mode != Mode::Recording || s.armed || opt.onDeath == OnDeath::Nothing) return;
    auto* pawn = game::Pawn();
    const bool dead = pawn && pawn->Health <= 0;
    if (!dead) {
        s.deathSeen = false;
        return;
    }
    if (s.deathSeen) return;
    s.deathSeen = true;
    if (g_replay.start == StartKind::Now) return;          // can't go back: the death stays in the macro

    if (opt.onDeath == OnDeath::Restart) {
        {
            std::lock_guard lock(g_mutex);
            g_replay.Truncate(0);
            ++g_replay.rerecords;
        }
        StopNow("died: restarting the attempt");
        Begin(Mode::Recording);
    } else {
        RewindNow("died: rewinding");
    }
}

// What differs between the player now and the recording ("velocity off by 0.002").
std::string DescribeDesync(const Sync& now, const Sync& want)
{
    if (!now.present || !want.present) return now.present ? "no player in the recording here" : "no player here";
    const auto len = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
    const float dp = len(now.x - want.x, now.y - want.y, now.z - want.z);
    const float dv = len(now.vx - want.vx, now.vy - want.vy, now.vz - want.vz);
    std::string out;
    char part[96];
    if (now.state != want.state) {
        std::snprintf(part, sizeof part, "move %u (recorded %u)", now.state, want.state);
        out += part;
    }
    if (dp > 0.f) {
        std::snprintf(part, sizeof part, "%sposition off by %.3f", out.empty() ? "" : ", ", dp);
        out += part;
    }
    if (dv > 0.f) {
        std::snprintf(part, sizeof part, "%svelocity off by %.3f", out.empty() ? "" : ", ", dv);
        out += part;
    }
    if (dp == 0.f && now.state == want.state) out += ", position matched";
    return out.empty() ? "differs" : out;
}

// OnDesync::Correct: the same move and close enough to put back on the recorded state.
bool CanCorrect(const Sync& now, const Sync& want)
{
    if (!now.present || !want.present || now.state != want.state) return false;
    const float dx = now.x - want.x, dy = now.y - want.y, dz = now.z - want.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= opt.correctMax;
}

void Correct(const Sync& want)
{
    auto* pawn = game::Pawn();
    if (!pawn) return;
    pawn->Location.X = want.x;          // written directly: SetLocation() doesn't move the player
    pawn->Location.Y = want.y;
    pawn->Location.Z = want.z;
    pawn->Velocity.X = want.vx;
    pawn->Velocity.Y = want.vy;
    pawn->Velocity.Z = want.vz;
}

// The player's state against the recording, called with g_mutex held. Frames past the checked range (edited) get their
// sync data refreshed instead, as long as everything before them matched. OnDesync decides what a mismatch does.
void CheckSync(const Sync& now)
{
    Replay& r = g_replay;
    if (r.start == StartKind::Now) return;
    if (r.sync.size() != r.frames.size()) r.sync.resize(r.frames.size());

    if (s.frame < r.syncValidUntil) {
        const Sync& want = r.sync[s.frame];
        if (want == now) {
            ++s.syncChecked;
        } else if (s.desyncFrame < 0 && opt.onDesync == OnDesync::Correct && CanCorrect(now, want)) {
            if (s.corrections == 0)
                spdlog::info("TAS: frame {} corrected ({}); further corrections are only counted", s.frame,
                             DescribeDesync(now, want));
            Correct(want);
            ++s.corrections;
            ++s.syncChecked;
        } else if (s.desyncFrame < 0) {
            s.desyncFrame = static_cast<long long>(s.frame);
            s.desyncInfo = DescribeDesync(now, want);
            spdlog::warn("TAS: desync at frame {} ({}): at ({:.3f}, {:.3f}, {:.3f}) v ({:.3f}, {:.3f}, {:.3f}) move {}, "
                         "recorded ({:.3f}, {:.3f}, {:.3f}) v ({:.3f}, {:.3f}, {:.3f}) move {}", s.frame, s.desyncInfo,
                         now.x, now.y, now.z, now.vx, now.vy, now.vz, now.state, want.x, want.y, want.z, want.vx,
                         want.vy, want.vz, want.state);
            g_status.message = "desync at frame " + std::to_string(s.frame) + ": " + s.desyncInfo;
            g_status.desyncInfo = s.desyncInfo;
            notify::Push("TAS: desync at frame " + std::to_string(s.frame) + ": " + s.desyncInfo);
            if (opt.onDesync == OnDesync::Pause) g_paused = true;
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
        g_status.verified = r.start != StartKind::Now && s.desyncFrame < 0 && s.corrections == 0 &&
                            checkable == r.frames.size() && s.syncChecked == checkable && checkable > 0;
    }
    switch (opt.atEnd) {
        case AtEnd::Record: TakeOverNow(); break;
        case AtEnd::Pause:  g_paused = true; SetMessage("end of the replay (paused)"); break;
        case AtEnd::Stop:   StopNow("end of the replay"); break;
        case AtEnd::Loop:
            RestartNow();
            SetMessage("looping");
            break;
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
    g_status.corrections = s.corrections;
}

// Arm: wait for the start level, running frames at the nominal step (they are the same in every run too).
// True when this frame was one of those (or the session gave up): OnFrame returns `result`. False once the level is
// there: frame 0 runs now.
bool ArmFrame(bool& result)
{
    if (s.sawLoad && !game::IsLoading() && game::Pawn()) {
        s.armed = false;
        ApplyMatch();                  // the level's own player input exists now
        s.vtime = kStartTime;      // frame 0 starts at the same clock in every run
        s.lastReal = engine::RealSeconds() - NominalDt(g_replay.fixedHz);   // the load isn't part of frame 0
        SetMessage(s.mode == Mode::Recording ? "recording" : "playing");
        return false;
    }
    if (!s.sawLoad && engine::RealSeconds() - s.armStart > kArmTimeout) {
        StopNow("the start level never loaded");
        result = false;
        return true;
    }
    const double dt = NominalDt(g_replay.fixedHz);
    s.vtime += dt;
    engine::SetFrameTime(s.vtime, dt);
    PublishStatus();
    result = true;
    return true;
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
    CheckDeath();
    static unsigned envTick = 0;
    if (++envTick % 30 == 1) PublishEnv();
    if (s.mode == Mode::Idle) return false;

    bool result = false;
    if (s.armed && ArmFrame(result)) return result;

    if (s.seekTo != kNoSeek && s.frame >= s.seekTo) {
        s.seekTo = kNoSeek;
        g_paused = true;
        SetRendering(true);
        if (s.recordAfterSeek) {
            s.recordAfterSeek = false;
            TakeOverNow();                       // paused: resume (or step) to keep recording
        }
    }
    if (WaitWhilePaused()) s.lastReal = engine::RealSeconds();
    if (s.mode == Mode::Idle) return false;
    // Restarted, or a playback started, from the menu while paused: its start level has only just been asked for.
    if (s.armed && ArmFrame(result)) return result;

    if (s.mode == Mode::Playing && s.frame >= g_replay.frames.size()) {
        if (!s.endReached) PlaybackEnded();         // stops, pauses, or switches to recording
        if (s.mode == Mode::Playing) {
            // Paused at the end: wait there. Resuming or stepping ends the playback, taking over keeps recording.
            PublishStatus();
            WaitWhilePaused();
            s.lastReal = engine::RealSeconds();
            if (s.armed && ArmFrame(result)) return result;     // restarted from the menu meanwhile
            if (s.mode == Mode::Playing && s.frame >= g_replay.frames.size()) StopNow("end of the replay");
        }
        if (s.mode == Mode::Idle) return false;
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
            // Camera keys: the look direction is set here, before the engine ticks, and the recorded look input
            // of these frames is left out.
            float camYaw = 0.f, camPitch = 0.f;
            const bool camera = timeline::CameraAt(r, s.frame, camYaw, camPitch);
            if (camera) ApplyCamera(camYaw, camPitch);
            const Frame& f = r.frames[s.frame];
            for (std::uint32_t i = 0; i < f.count; ++i) {
                const Event& e = r.events[f.first + i];
                if (camera && e.kind == EventKind::Axis && timeline::IsLookAxis(r.Name(e.name))) continue;
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

// The example macros built into the DLL go to replays/ once: replays/.examples lists the ones written before, so an
// example that was deleted or renamed doesn't come back.
void InstallExamples()
{
    std::error_code ec;
    const std::filesystem::path folder = Folder();
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path listPath = folder / ".examples";

    std::set<std::string> done;
    {
        std::ifstream list(listPath);
        for (std::string line; std::getline(list, line);)
            if (!line.empty()) done.insert(line);
    }
    bool changed = false;
    for (const examples::File& f : examples::All()) {
        if (!f.name || done.count(f.name)) continue;
        const std::filesystem::path path = folder / f.name;
        if (!std::filesystem::exists(path, ec)) {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
            if (!out) {
                spdlog::warn("TAS: couldn't write the example macro {}", path.string());
                continue;
            }
            spdlog::info("TAS: example macro {} added", path.string());
        }
        done.insert(f.name);
        changed = true;
    }
    if (changed) {
        std::ofstream list(listPath, std::ios::trunc);
        for (const auto& name : done) list << name << '\n';
    }
}

} // namespace

// ---- public ----------------------------------------------------------------------------------------------------------------
bool Init()
{
    InstallExamples();
    if (!engine::Init()) return false;
    engine::SetFrameHandler(&OnFrame);
    engine::SetInputFilter(&FilterInput);
    input::Install();               // What the engine gets, for the input display
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
        float hz;
        {
            std::lock_guard lock(g_optMutex);
            hz = g_options.recordHz;
        }
        RecordNow(start, command, map, checkpoint, hz);
    });
}

void Restart() { Post([] { RestartNow(); }); }

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
            // A macro that starts "now" starts wherever the player is: from idle that's fine, but a running one
            // can't get back to where it began.
            if (start == StartKind::Now && s.mode != Mode::Idle) {
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

void Rewind() { Post([] { RewindNow("rewound"); }); }

std::vector<Issue> Preflight()
{
    using Level = Issue::Level;
    Options o;
    {
        std::lock_guard lock(g_optMutex);
        o = g_options;
    }
    Env now;
    {
        std::lock_guard lock(g_envMutex);
        now = g_envShared;
    }
    std::vector<Issue> out;
    std::lock_guard lock(g_mutex);
    const Replay& r = g_replay;
    if (r.frames.empty()) return out;

    if (r.start == StartKind::Now)
        out.push_back({Level::Info, "Starts \"now\": a playback can't be checked against the recording", false});
    if (r.gameStamp && r.gameStamp != engine::GameStamp())
        out.push_back({Level::Problem, "Made with a different version of the game: it will likely desync", false});
    if (!HasExt(r, "blockStreaming")) {
        out.push_back({Level::Info, "Recorded before the menu saved its settings with macros: nothing to compare", false});
        return out;
    }

    const bool recStreaming = Ext(r, "blockStreaming") == "1";
    if (recStreaming != o.blockStreaming)
        out.push_back({Level::Warning, std::string("Recorded with deterministic streaming ") +
                                           (recStreaming ? "on" : "off") + ", it is " + (o.blockStreaming ? "on" : "off"),
                       o.matchRecording});

    const std::string recMods = Ext(r, "mods");
    if (recMods != o.gameplayMods)
        out.push_back({Level::Warning, "Modules that change the game: " +
                                           (o.gameplayMods.empty() ? std::string("none on now") : "on now: " + o.gameplayMods) +
                                           "; " + (recMods.empty() ? std::string("none when recorded")
                                                                   : "when recorded: " + recMods),
                       false});

    if (!now.valid) {
        out.push_back({Level::Info, "No player: mouse settings and key bindings are checked once in a level", false});
        return out;
    }
    if (HasExt(r, "sensitivity")) {
        const float sens = std::strtof(Ext(r, "sensitivity").c_str(), nullptr);
        const bool invert = Ext(r, "invertMouse") == "1";
        char text[160];
        if (std::abs(sens - now.sensitivity) > 1e-4f) {
            std::snprintf(text, sizeof text, "Mouse sensitivity is %.2f, the macro was recorded with %.2f",
                          now.sensitivity, sens);
            out.push_back({Level::Warning, text, o.matchRecording});
        }
        if (invert != now.invertMouse)
            out.push_back({Level::Warning, std::string("Invert mouse is ") + (now.invertMouse ? "on" : "off") +
                                               ", it was " + (invert ? "on" : "off") + " when recorded",
                           o.matchRecording});
    }
    for (const std::string& name : r.names) {
        if (!HasExt(r, "bind." + name)) continue;
        const std::string recorded = Ext(r, "bind." + name);
        const auto cur = now.binds.find(name);
        if (cur != now.binds.end() && cur->second == recorded) continue;
        const std::string key = KeyFor(now, recorded, name);
        const std::string does = cur == now.binds.end() ? std::string("nothing") : "\"" + cur->second + "\"";
        if (key.empty())
            out.push_back({Level::Problem, name + " does " + does + " now; when recorded it was \"" + recorded +
                                               "\", which no key does now", false});
        else
            out.push_back({Level::Warning, name + " does " + does + " now: played as " + key + " (\"" + recorded + "\")",
                           o.matchRecording});
    }
    return out;
}

void ResetSync()
{
    Post([] {
        std::lock_guard lock(g_mutex);
        g_replay.sync.clear();
        g_replay.syncValidUntil = 0;
    });
    SetMessage("the next playback records the reference states again", true);
}

void KeyCameraFromView(std::uint32_t frame)
{
    Post([frame] {
        float yaw = 0.f, pitch = 0.f;
        if (!ReadCamera(yaw, pitch)) {
            SetMessage("no player camera to take the key from", true);
            return;
        }
        std::lock_guard lock(g_mutex);
        if (frame >= g_replay.frames.size()) return;
        // Turn the shorter way from the camera around it (the key it replaces, else the keys' path there)
        CameraKey key;
        key.frame = frame;
        float nearYaw = yaw, nearPitch = 0.f;
        if (const CameraKey* old = timeline::FindCameraKey(g_replay, frame)) {
            key.ease = old->ease;
            nearYaw = old->yaw;
        } else if (!timeline::CameraAt(g_replay, frame, nearYaw, nearPitch) && !g_replay.camera.empty()) {
            nearYaw = frame < g_replay.camera.front().frame ? g_replay.camera.front().yaw : g_replay.camera.back().yaw;
        }
        key.yaw = timeline::UnwrapNear(yaw, nearYaw);
        key.pitch = pitch;
        timeline::SetCameraKey(g_replay, key);
    });
}

void Clear()
{
    Post([] {
        StopNow("cleared");
        std::lock_guard lock(g_mutex);
        g_replay.Clear();
        g_savedHash = ContentHash(g_replay);
        s.resolved.clear();
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
    const bool json = path.extension() == ".json";
    if (!(json ? r.ImportJson(path, error) : r.Load(path, error))) return false;
    if (r.gameStamp && r.gameStamp != engine::GameStamp())
        SetMessage("warning: made with a different game version, it will likely desync", true);
    // Not while a session uses the replay: stop it first, then swap.
    Post([r = std::move(r)]() mutable {
        StopNow("stopped");
        {
            std::lock_guard lock(g_mutex);
            g_replay = std::move(r);
            g_savedHash = ContentHash(g_replay);
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
    const bool ok = path.extension() == ".json" ? g_replay.ExportJson(path, error) : g_replay.Save(path, error);
    if (ok) g_savedHash = ContentHash(g_replay);
    return ok;
}

std::filesystem::path Folder() { return paths::Dir() / "replays"; }
std::filesystem::path AutosaveFolder() { return Folder() / "autosave"; }

bool Unsaved()
{
    std::lock_guard lock(g_mutex);
    return !g_replay.frames.empty() && ContentHash(g_replay) != g_savedHash;
}

} // namespace tas
