#include "features/player/fly_module.h"
#include "game/game.h"
#include "gui.h"

FlyModule::FlyModule(std::string name, std::string description, int order, flight::Mode mode)
    : Module(std::move(name), "Player", std::move(description), order), mode_(mode)
{
    group = "movement";
    schedule.priority = sched::Priority::Critical;
}

flight::Settings FlyModule::Current() const
{
    flight::Settings s;
    s.speed = speed;
    s.boost = boost;
    s.responsiveness = responsiveness;
    s.followView = followView;
    s.invulnerable = invulnerable;
    s.blockActions = blockActions;
    s.inputEnabled = !gui::visible;               // menu open: hover, don't fly off
    s.up = upKey.Down();
    s.down = downKey.Down();
    s.boostHeld = boostKey.Down();
    return s;
}

void FlyModule::OnTick() { flight::Tick(mode_, Current()); }

void FlyModule::OnDisable()
{
    // OnDisable can run on the menu thread: hand the cleanup to the game thread.
    const flight::Mode mode = mode_;
    game::RunOnGameThread([mode] { flight::Stop(mode); });
}
