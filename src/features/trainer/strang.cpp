#include "features/trainer/tool.h"

namespace {

class Strang final : public TrainerTool {
public:
    Strang() : TrainerTool("Strang", "Arm: jumps on the exact frame your next move starts", 30)
    {
        instance_ = this;
        game::OnPlayerStateChange(&OnStateChange);
    }

    ~Strang() override { instance_ = nullptr; }

protected:
    bool Step(Classes::ATdPlayerPawn*, Classes::ATdPlayerController*) override { return false; }   // waits for the hook

private:
    // Game thread, from the engine's move state handler: jump instead of letting it handle the new state.
    static bool OnStateChange()
    {
        Strang* self = instance_;
        if (!self || !self->Enabled()) return false;
        auto* controller = game::Controller();
        if (controller && controller->PlayerInput) controller->PlayerInput->Jump();
        self->SetEnabled(false);
        return true;
    }

    static inline Strang* instance_ = nullptr;
};
MEM_MODULE(Strang)

} // namespace
