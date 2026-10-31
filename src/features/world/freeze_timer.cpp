#include "core/module.h"

namespace {

class FreezeTimer final : public Module {
public:
    FreezeTimer() : Module("Freeze Timer", "World", "Stop the time trial clock", 20) {}

    void OnTick() override
    {
        // TODO: needs the time trial's clock (not found in the SDK yet).
    }

private:
    ctl::Label note{this, "Only affects time trials. Not implemented yet."};
};
MEM_MODULE(FreezeTimer)

} // namespace
