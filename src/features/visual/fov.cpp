#include "core/module.h"
#include "game/game.h"

#include <ME_SDK.hpp>

namespace {

// Locks the player camera's field of view (the engine's own "locked FOV": the camera uses it instead of the FOV its
// modes ask for, zoom included). Put back as it was when switched off.
class Fov final : public Module {
public:
    Fov() : Module("FOV", "Visual", "Override the field of view", 10)
    {
        schedule.priority = sched::Priority::High;   // Every tick: a level change brings a new camera
    }

    void OnTick() override
    {
        auto* pc = game::Controller();
        if (!pc || !pc->PlayerCamera) return;
        auto* cam = pc->PlayerCamera;
        if (cam != camera_) {       // First tick, or a new camera (level change, respawn): remember its own values
            camera_ = cam;
            savedLocked_ = cam->bLockedFOV;
            savedLockedFov_ = cam->LockedFOV;
            savedDefault_ = cam->DefaultFOV;
        }
        cam->bLockedFOV = 1;
        cam->LockedFOV = degrees;
        cam->DefaultFOV = degrees;
    }

    void OnDisable() override
    {
        // May run on the render thread: put the camera back on the game thread.
        game::RunOnGameThread([this] {
            auto* pc = game::Controller();
            if (pc && pc->PlayerCamera && pc->PlayerCamera == camera_) {
                camera_->bLockedFOV = savedLocked_;
                camera_->LockedFOV = savedLockedFov_;
                camera_->DefaultFOV = savedDefault_;
            }
            camera_ = nullptr;
        });
    }

private:
    ctl::Slider degrees{this, "Degrees", 90.f, 60.f, 130.f, "%.0f", 5.f};

    // Game thread
    Classes::ACamera* camera_ = nullptr;
    unsigned long savedLocked_ = 0;
    float savedLockedFov_ = 0.f, savedDefault_ = 90.f;
};
MEM_MODULE(Fov)

} // namespace
