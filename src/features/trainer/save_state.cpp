#include "core/module.h"
#include "core/notify.h"
#include "game/game.h"
#include "game/player_state.h"

#include <mirrorsedge_sdk.h>

namespace {

class SaveState final : public ActionModule {
public:
    SaveState() : ActionModule("Save State", "Trainer", "Remember where you are, how fast you move and what you are doing", 50)
    {
        key = '4';
    }

    void OnTrigger() override
    {
        game::RunOnGameThread([] {
            if (!game::Pawn()) notify::Push("Save State: no player");
            else if (!player_state::Save()) notify::Push("Save State: failed (see the log)");
            else notify::Push("State saved");
        });
    }

private:
    ctl::Button clear{this, "Clear saved state", [] { game::RunOnGameThread([] { player_state::Clear(); }); }};
};
MEM_MODULE(SaveState)

class LoadState final : public ActionModule {
public:
    LoadState() : ActionModule("Load State", "Trainer", "Go back to the saved state", 51)
    {
        key = '5';
    }

    void OnTrigger() override
    {
        game::RunOnGameThread([] {
            if (!player_state::HasSave()) {
                notify::Push("Load State: nothing saved yet");
                return;
            }
            auto* pawn = game::Pawn();
            if (!pawn || pawn->Health <= 0) return;

            // An armed Strang or Beamer would fire right away in the loaded state (the trainer drops them too).
            for (const char* tool : {"Strang", "Beamer"})
                if (Module* m = modules::Find(tool)) m->SetEnabled(false);

            if (!player_state::Load()) notify::Push("Load State: not right now");
        });
    }
};
MEM_MODULE(LoadState)

class RefillReactionTime final : public ActionModule {
public:
    RefillReactionTime() : ActionModule("Refill Reaction Time", "Trainer", "Fill the reaction time meter", 60) {}

    void OnTrigger() override
    {
        game::RunOnGameThread([] {
            auto* pawn = game::Pawn();
            auto* controller = game::Controller();
            auto* world = game::World();
            if (!pawn || !controller || !world) return;

            // Same conditions as the trainer's button: not in time trials, not already full or active, not falling to death.
            if (world->Game && world->Game->IsA(Classes::ATdSPTimeTrialGame::StaticClass())) {
                notify::Push("Reaction time: not in time trials");
                return;
            }
            if (controller->ReactionTimeEnergy >= 100.f || controller->bReactionTime ||
                pawn->MovementState == Classes::EMovement::MOVE_FallingUncontrolled)
                return;

            controller->ReactionTimeEnergy = 100.f;
            if (auto* hud = static_cast<Classes::ATdHUD*>(controller->myHUD); hud && hud->EffectManager)
                hud->EffectManager->ActivateReactionTimeTeaser();
        });
    }
};
MEM_MODULE(RefillReactionTime)

} // namespace
