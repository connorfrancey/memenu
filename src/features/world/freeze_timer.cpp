#include "core/module.h"
#include "game/game.h"
#include "game/uobject.h"

#include <ME_SDK.hpp>

namespace {

// Stops the time trial clock. The race time is the world time minus the race's start stamp (and the split times
// count from the last checkpoint's stamp), so while frozen both stamps move forward by the world time that passed.
class FreezeTimer final : public Module {
public:
    FreezeTimer() : Module("Freeze Timer", "World", "Stop the time trial clock", 20)
    {
        schedule.priority = sched::Priority::Critical;     // Every tick, or the clock creeps on
    }

    void OnTick() override
    {
        auto* world = game::World();
        auto* tt = world && world->Game && game::IsA(world->Game, Classes::ATdSPTimeTrialGame::StaticClass())
                       ? static_cast<Classes::ATdSPTimeTrialGame*>(world->Game) : nullptr;
        if (!tt) { last_ = -1.f; return; }

        const float now = world->TimeSeconds;
        const bool running = tt->RaceStartTimeStamp > 0.f && tt->RaceEndTimeStamp <= 0.f;
        if (running && last_ >= 0.f && now > last_) {
            const float passed = now - last_;
            tt->RaceStartTimeStamp += passed;
            if (tt->LastCheckpointTimeStamp > 0.f) tt->LastCheckpointTimeStamp += passed;
        }
        last_ = now;
    }

    void OnDisable() override { game::RunOnGameThread([this] { last_ = -1.f; }); }   // May run on the render thread

private:
    ctl::Label note{this, "Only affects time trials."};
    float last_ = -1.f;     // Game thread: world time at the previous tick
};
MEM_MODULE(FreezeTimer)

} // namespace
