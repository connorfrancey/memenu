#include "core/module.h"
#include "core/notify.h"
#include "game/flight.h"
#include "game/game.h"
#include "gui.h"
#include "features/trainer/kickglitch_helper.h"
#if MEMENU_FEATURE_TAS
#include "tas/input_monitor.h"
#endif

#include <imgui.h>
#include "sdk/SDK.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

using Classes::EMovement;
using Classes::EPhysics;

std::mutex g_lastMutex;
kickglitch_helper::Last g_last;

// Timing practice for the kickglitch: kick during a wallrun (attack), and jump on the frame you land. The helper sees
// each attempt by itself (the game's wallrun kick move), and says how many frames from that frame the jump came:
//
//   kick (MOVE_MeleeWallrun) -> lands (physics walking) -> jump (MOVE_Jump), counted in engine ticks (= frames)
//
// "Perfect" is the frame the Kickglitch tool's own jump lands on. That is calibrated by doing one attempt with the tool
// armed (the result becomes the target), and starts at 1: the tool jumps in the tick it sees the landing, which the
// game turns into a jump in the next one.
//
// Jump presses before the landing (which the game ignores during the kick) are seen through the engine's input
// monitor, so an attempt with no jump at all can still say how early the press was.
class KickglitchHelper final : public Module {
public:
    KickglitchHelper()
        : Module("Kickglitch Helper", "Trainer", "Times your kickglitches: how many frames from the perfect jump you were", 11)
    {
        schedule.priority = sched::Priority::Critical;     // must see every tick: the window is one frame
    }

    void OnEnable() override
    {
        {
            std::lock_guard lock(g_lastMutex);
            g_last = {};
        }
        game::RunOnGameThread([this] {
            attempt_ = {};
            keysAt_ = -1000000;
        });
    }

    void OnTick() override
    {
        ++tick_;
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        if (!pawn || !controller || pawn->Health <= 0 || flight::Active()) {
            attempt_ = {};
            return;
        }
        if (tick_ - keysAt_ > 300) FindJumpKeys(controller);    // Bindings can change in the options

        const auto move = pawn->MovementState.GetValue();
        const auto physics = pawn->Physics.GetValue();
        const float speed = std::hypot(pawn->Velocity.X, pawn->Velocity.Y);

        switch (attempt_.phase) {
            case Phase::Idle:
                if (move == EMovement::MOVE_MeleeWallrun) {
                    attempt_ = {};
                    attempt_.phase = Phase::Kicking;
                    attempt_.kickTick = tick_;
                    attempt_.speedBefore = lastSpeed_;
                    if (physics == EPhysics::PHYS_Walking) {   // Kicked right above the ground: landed already
                        attempt_.landTick = tick_;
                        attempt_.phase = Phase::Landed;
                        std::lock_guard lock(mutex_);
                        landedAt_ = Seconds();
                    }
                    attempt_.calibrating = ToolArmed() || toolWasArmed_;
                    jumpPresses_ = JumpPresses();
                }
                break;

            case Phase::Kicking:
                WatchPresses();
                attempt_.calibrating |= ToolArmed() || toolWasArmed_;
                if (physics == EPhysics::PHYS_Walking) {
                    attempt_.landTick = tick_;
                    attempt_.phase = Phase::Landed;
                    std::lock_guard lock(mutex_);
                    landedAt_ = Seconds();                      // The flash (render thread reads it)
                } else if (tick_ - attempt_.kickTick > 240 || IsOtherMove(move)) {
                    attempt_ = {};                              // Grabbed something, wallran again, ...: not an attempt
                }
                break;

            case Phase::Landed:
                WatchPresses();
                attempt_.calibrating |= ToolArmed() || toolWasArmed_;
                // The jump move, or (whatever the move says) back in the air going up: the jump impulse
                if (move == EMovement::MOVE_Jump || (physics == EPhysics::PHYS_Falling && pawn->Velocity.Z > 100.f)) {
                    attempt_.jumpTick = tick_;
                    attempt_.phase = Phase::Measuring;
                } else if (tick_ - attempt_.landTick > kLateLimit) {
                    Finish(pawn);                               // No jump at all
                }
                break;

            case Phase::Measuring:                              // The speed the jump carries, a few frames in
                if (tick_ - attempt_.jumpTick >= 3) Finish(pawn);
                break;
        }
        lastSpeed_ = speed;
        // The tool switches itself off in the tick it jumps, maybe before this one ran: remember it was armed
        toolWasArmed_ = ToolArmed();
    }

    void OnRender() override
    {
        Shown v;
        double landedAt;
        {
            std::lock_guard lock(mutex_);
            v = shown_;
            landedAt = landedAt_;
        }

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
        if (!gui::visible) flags |= ImGuiWindowFlags_NoInputs;
        ImGui::SetNextWindowPos({60.f, 320.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(opacity);
        if (ImGui::Begin("Kickglitch Helper##trainer", nullptr, flags)) {
            ImGui::TextDisabled("Kickglitch");
            if (v.attempts == 0) {
                ImGui::TextUnformatted("Wallrun, kick, jump as you land");
            } else {
                ImGui::TextColored(ColorFor(v.last), "%s", v.lastText.c_str());
                if (!v.lastDetail.empty()) ImGui::TextDisabled("%s", v.lastDetail.c_str());
            }
            DrawBar(v);
            if (v.attempts > 0)
                ImGui::Text("%d / %d perfect (%.0f%%)  streak %d, best %d", v.perfect, v.attempts,
                            100.0 * v.perfect / v.attempts, v.streak, v.bestStreak);
        }
        // Flash the window's border on the landing frame: shows when the jump should have come
        if (flashOnLanding && Seconds() - landedAt < 0.12) {
            const ImVec2 a = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            ImGui::GetForegroundDrawList()->AddRect(a, {a.x + size.x, a.y + size.y}, IM_COL32(90, 200, 255, 255), 4.f, 0, 3.f);
        }
        ImGui::End();
    }

private:
    static constexpr int kLateLimit = 20;       // Frames after landing: past this it was no jump
    static constexpr int kBarFrames = 4;        // The bar shows -4 .. +4 around perfect

    enum class Phase { Idle, Kicking, Landed, Measuring };
    enum class Result { None, Perfect, Close, Off, NoJump };

    struct Attempt {
        Phase phase = Phase::Idle;
        long long kickTick = 0, landTick = 0, jumpTick = -1, firstPress = -1;
        float speedBefore = 0.f;
        bool calibrating = false;
    };

    struct Shown {
        int attempts = 0, perfect = 0, streak = 0, bestStreak = 0;
        Result last = Result::None;
        std::string lastText, lastDetail;
        std::deque<int> history;                // Offsets from perfect, newest last (NoJump not included)
        int target = 1;
    };

    // ---- game thread ---------------------------------------------------------------------------------------------
    void Finish(Classes::ATdPlayerPawn* pawn)
    {
        const Attempt a = attempt_;
        attempt_ = {};
        const float speedAfter = std::hypot(pawn->Velocity.X, pawn->Velocity.Y);

        if (a.calibrating && a.jumpTick >= 0) {
            const int frame = static_cast<int>(a.jumpTick - a.landTick);
            perfectFrame.Set(std::clamp(frame, 0, 5));
            notify::Push("Kickglitch Helper: perfect is the jump " + std::to_string(frame) + " frame(s) after landing");
        }
        {   // After the calibration: whoever waits on the count sees it done
            std::lock_guard lock(g_lastMutex);
            ++g_last.attempts;
            g_last.jumpFrame = a.jumpTick >= 0 ? static_cast<int>(a.jumpTick - a.landTick) : -1;
            g_last.calibrated = a.calibrating && a.jumpTick >= 0;
        }

        std::lock_guard lock(mutex_);
        Shown& s = shown_;
        s.target = perfectFrame;
        ++s.attempts;
        char detail[96];
        if (a.jumpTick < 0) {
            s.last = Result::NoJump;
            s.lastText = "No jump";
            if (a.firstPress >= 0 && a.firstPress <= a.landTick) {
                const auto early = a.landTick - a.firstPress;
                std::snprintf(detail, sizeof detail, "pressed %lld frame%s before landing", early, early == 1 ? "" : "s");
                s.lastDetail = detail;
            } else {
                s.lastDetail = "within " + std::to_string(kLateLimit) + " frames of landing";
            }
            s.streak = 0;
            return;
        }

        const int offset = static_cast<int>(a.jumpTick - a.landTick) - s.target;
        s.history.push_back(offset);
        while (s.history.size() > 12) s.history.pop_front();
        if (offset == 0) {
            s.last = Result::Perfect;
            s.lastText = "PERFECT";
            ++s.perfect;
            s.bestStreak = std::max(s.bestStreak, ++s.streak);
        } else {
            s.last = std::abs(offset) == 1 ? Result::Close : Result::Off;
            const int n = std::abs(offset);
            s.lastText = std::to_string(n) + (n == 1 ? " frame " : " frames ") + (offset < 0 ? "early" : "late");
            s.streak = 0;
        }
        std::snprintf(detail, sizeof detail, "speed %.0f -> %.0f u/s", a.speedBefore, speedAfter);
        s.lastDetail = detail;
    }

    static double Seconds()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // Leaving the kick for something that is not a landing
    static bool IsOtherMove(EMovement m)
    {
        switch (m) {
            case EMovement::MOVE_Jump:
            case EMovement::MOVE_WallRunJump:
            case EMovement::MOVE_Grabbing:
            case EMovement::MOVE_WallRunningLeft:
            case EMovement::MOVE_WallRunningRight:
            case EMovement::MOVE_WallClimbing:
            case EMovement::MOVE_IntoGrab:
            case EMovement::MOVE_ZipLine:
            case EMovement::MOVE_FallingUncontrolled:
                return true;
            default:
                return false;
        }
    }

    static bool ToolArmed()
    {
        Module* tool = modules::Find("Kickglitch");
        return tool && tool->Enabled();
    }

    // Keys bound to jump, in the input monitor's spelling (lower case engine names)
    void FindJumpKeys(Classes::ATdPlayerController* controller)
    {
        keysAt_ = tick_;
        jumpKeys_.clear();
        if (!controller->PlayerInput) return;
        auto& binds = controller->PlayerInput->Bindings;
        for (std::size_t i = 0; i < binds.Num(); ++i) {
            const auto& b = binds[i];
            if (!b.Command.IsValid()) continue;
            const std::string cmd = b.Command.ToString();
            std::string name = b.Name.GetName();
            if (name.rfind("GBA_", 0) == 0) continue;           // An alias, not a key
            if (cmd.rfind("GBA_Jump", 0) != 0) continue;
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            jumpKeys_.push_back(std::move(name));
        }
    }

    std::uint32_t JumpPresses() const
    {
#if MEMENU_FEATURE_TAS
        if (jumpKeys_.empty() || !tas::input::Installed()) return 0;
        const auto snap = tas::input::Get();
        std::uint32_t n = 0;
        for (const std::string& k : jumpKeys_)
            if (const auto it = snap.keys.find(k); it != snap.keys.end()) n += it->second.presses;
        return n;
#else
        return 0;       // No input monitor in this build: early presses aren't seen
#endif
    }

    void WatchPresses()
    {
        if (attempt_.firstPress >= 0) return;
        const std::uint32_t n = JumpPresses();
        if (n > jumpPresses_) attempt_.firstPress = tick_;
        jumpPresses_ = n;
    }

    // ---- render thread -------------------------------------------------------------------------------------------
    static ImVec4 ColorFor(Result r)
    {
        switch (r) {
            case Result::Perfect: return {0.35f, 0.9f, 0.4f, 1.f};
            case Result::Close:   return {1.f, 0.75f, 0.25f, 1.f};
            default:              return {1.f, 0.4f, 0.35f, 1.f};
        }
    }

    // A row of cells, one per frame around perfect (the middle one). Older results fade, the newest is outlined.
    void DrawBar(const Shown& v)
    {
        const float cell = 18.f, gap = 3.f, h = 14.f;
        const int cells = kBarFrames * 2 + 1;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        for (int i = 0; i < cells; ++i) {
            const int offset = i - kBarFrames;
            const ImVec2 a{p.x + i * (cell + gap), p.y};
            const ImVec2 b{a.x + cell, a.y + h};
            const ImU32 base = offset == 0 ? IM_COL32(60, 140, 70, 255) : IM_COL32(70, 70, 76, 255);
            dl->AddRectFilled(a, b, base, 2.f);
            // How often the recent attempts landed here
            int hits = 0;
            for (int o : v.history) hits += std::clamp(o, -kBarFrames, kBarFrames) == offset;
            if (hits) {
                const int alpha = std::min(255, 70 + hits * 50);
                dl->AddRectFilled(a, b, offset == 0 ? IM_COL32(90, 230, 110, alpha) : IM_COL32(240, 170, 60, alpha), 2.f);
            }
            if (!v.history.empty() && std::clamp(v.history.back(), -kBarFrames, kBarFrames) == offset)
                dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, IM_COL32(255, 255, 255, 255), 2.f, 0, 2.f);
        }
        ImGui::Dummy({cells * (cell + gap) - gap, h});
        ImGui::TextDisabled("early          perfect          late");
    }

    void ResetStats()
    {
        std::lock_guard lock(mutex_);
        shown_ = {};
        shown_.target = perfectFrame;
    }

    // Game thread
    long long tick_ = 0, keysAt_ = -1000000;
    Attempt attempt_;
    float lastSpeed_ = 0.f;
    bool toolWasArmed_ = false;
    std::vector<std::string> jumpKeys_;
    std::uint32_t jumpPresses_ = 0;

    std::mutex mutex_;
    Shown shown_;               // Written by the game thread, drawn by the render thread
    double landedAt_ = -1.0;

    ctl::Checkbox flashOnLanding{this, "Flash on landing", true, "Outline the window on the frame you land"};
    ctl::SliderInt perfectFrame{this, "Perfect frame", 1, 0, 5, "%d", 1,
                                "How many frames after landing the perfect jump starts. Arm the Kickglitch tool and do "
                                "one kick to measure it"};
    ctl::Slider opacity{this, "Opacity", 0.6f, 0.f, 1.f, "%.2f", 0.05f};
    ctl::Button reset{this, "Reset stats", [this] { ResetStats(); }};
};
MEM_MODULE(KickglitchHelper)

} // namespace

kickglitch_helper::Last kickglitch_helper::Get()
{
    std::lock_guard lock(g_lastMutex);
    return g_last;
}
