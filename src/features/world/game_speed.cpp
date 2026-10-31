#include "core/module.h"
#include "game/game.h"

#include <ME_SDK.hpp>

namespace {

class GameSpeed final : public Module {
public:
    GameSpeed() : Module("Game Speed", "World", "Change the world time dilation", 10)
    {
        schedule.priority = sched::Priority::Critical;
    }

    void OnTick() override
    {
        if (auto* world = game::World()) {
            const float speed = multiplier;
            world->TimeDilation = speed;
            if (world->Game) world->Game->GameSpeed = speed;
        }
    }

    void OnDisable() override
    {
        // OnDisable can run on the render thread: hand the cleanup to the game thread.
        game::RunOnGameThread([] {
            if (auto* world = game::World()) {
                world->TimeDilation = 1.f;
                if (world->Game) world->Game->GameSpeed = 1.f;
            }
        });
    }

private:
    ctl::Slider multiplier{this, "Multiplier", 1.f, 0.1f, 3.f, "%.2fx", 0.1f};
    ctl::Button reset{this, "Reset to 1.0x", [this] { multiplier.Set(1.f); }};
};
MEM_MODULE(GameSpeed)

} // namespace
