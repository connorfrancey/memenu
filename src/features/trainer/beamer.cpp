#include "features/trainer/tool.h"
#include "core/notify.h"

#include <Windows.h>

#include <atomic>
#include <cmath>

namespace {

class Beamer final : public TrainerTool {
public:
    Beamer() : TrainerTool("Beamer", "Arm while wall climbing: turns around (or sidesteps) at full climb speed", 20)
    {
        automatic.VisibleIf([this] { return sidestep.Get(); });
        direction.VisibleIf([this] { return sidestep.Get() && automatic.Get(); });
    }

    void OnEnable() override { started_ = false; }

    void OnDisable() override { ReleaseKey(); }   // SendInput is fine from any thread

protected:
    bool Step(Classes::ATdPlayerPawn* pawn, Classes::ATdPlayerController* controller) override
    {
        if (pawn->MovementState != Classes::EMovement::MOVE_WallClimbing) {
            if (!started_) notify::Push("Beamer: not wall climbing");
            return true;
        }
        if (!started_) {
            started_ = true;
            if (sidestep && automatic) PressKey(direction.Get() == 0 ? leftKey.Get() : rightKey.Get());
        }

        if (std::hypot(pawn->Velocity.X, pawn->Velocity.Y) <= 1000.f) return false;
        if (sidestep) controller->PlayerInput->Jump();
        else          controller->LookBehind();
        return true;
    }

private:
    // The automatic sidestep holds a strafe key through the climb, like the trainer does (a real key event, so the game
    // reads it the same way as your own).
    void PressKey(int vk)
    {
        if (!vk) return;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = static_cast<WORD>(vk);
        SendInput(1, &in, sizeof in);
        pressed_ = vk;
    }

    void ReleaseKey()
    {
        const int vk = pressed_.exchange(0);
        if (!vk) return;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = static_cast<WORD>(vk);
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof in);
    }

    bool started_ = false;
    std::atomic<int> pressed_{0};

    ctl::Checkbox sidestep{this, "Sidestep beamer", false, "Jump sideways off the wall instead of turning around"};
    ctl::Checkbox automatic{this, "Auto sidestep", false,
                            "Hold the strafe key for you. Off: hold left or right yourself while the beamer is armed"};
    ctl::Combo direction{this, "Direction", {"Left", "Right"}};
    ctl::KeyHold leftKey{this, "Left key", 'A', "Your strafe left key (pressed by Auto sidestep)"};
    ctl::KeyHold rightKey{this, "Right key", 'D', "Your strafe right key (pressed by Auto sidestep)"};
};
MEM_MODULE(Beamer)

} // namespace
