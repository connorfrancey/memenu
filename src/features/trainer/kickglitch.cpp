#include "features/trainer/tool.h"

namespace {

class Kickglitch final : public TrainerTool {
public:
    Kickglitch() : TrainerTool("Kickglitch", "Arm, then wallrun: kicks off the wall and jumps the moment you land", 10) {}

protected:
    bool Step(Classes::ATdPlayerPawn* pawn, Classes::ATdPlayerController* controller) override
    {
        if (pawn->Physics == Classes::EPhysics::PHYS_Walking) {
            controller->PlayerInput->Jump();
            return true;
        }
        if (pawn->Physics == Classes::EPhysics::PHYS_WallRunning) {
            controller->AttackPress();
            controller->AttackRelease();
        }
        return false;
    }
};
MEM_MODULE(Kickglitch)

} // namespace
