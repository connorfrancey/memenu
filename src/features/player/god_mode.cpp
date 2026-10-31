#include "core/module.h"
#include "game/game.h"
#include "game/player_state.h"

#include <mirrorsedge_sdk.h>

namespace {

class GodMode final : public Module {
public:
    GodMode() : Module("God Mode", "Player", "Take no damage", 10) { schedule.priority = sched::Priority::Critical; }

    void OnEnable() override
    {
        // Like the trainer's God tool: switched on during a death fall, it catches you.
        game::RunOnGameThread([] {
            auto* pawn = game::Pawn();
            auto* controller = game::Controller();
            if (pawn && controller && pawn->Health > 0 &&
                pawn->MovementState == Classes::EMovement::MOVE_FallingUncontrolled)
                player_state::ResetMovement(pawn, controller);
        });
    }

    void OnTick() override
    {
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        if (!pawn || !controller) return;

        controller->bGodMode = 1;               // engine-level "God" flag
        pawn->Health = pawn->MaxHealth;         // what mmultiplayer's trainer does...
        pawn->EnterFallingHeight = -1e30f;      // ...plus no fall deaths (also from the trainer)
    }

    void OnDisable() override
    {
        game::RunOnGameThread([] {
            if (auto* controller = game::Controller()) controller->bGodMode = 0;
        });
    }
};
MEM_MODULE(GodMode)

} // namespace
