#pragma once
#include "core/module.h"
#include "game/flight.h"
#include "game/game.h"

#include <ME_SDK.hpp>

#include <string>
#include <utility>

// A one-shot tool from mmultiplayer's trainer (Kickglitch, Beamer, Strang): switching it on arms it, and it switches
// itself off once it has done its move. Like in the trainer it is also disarmed when the player dies, the level
// changes or Flight/No Clip is on.
class TrainerTool : public Module {
public:
    TrainerTool(std::string name, std::string description, int order) : Module(std::move(name), "Trainer", std::move(description), order)
    {
        schedule.priority = sched::Priority::Critical;     // fires on an exact frame: never deferred
    }

    void OnTick() final
    {
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        if (!pawn || !controller || !controller->PlayerInput || pawn->Health <= 0 || flight::Active()) {
            SetEnabled(false);
            return;
        }
        if (Step(pawn, controller)) SetEnabled(false);
    }

protected:
    // Game thread, every tick while armed. Return true when done.
    virtual bool Step(Classes::ATdPlayerPawn* pawn, Classes::ATdPlayerController* controller) = 0;
};
