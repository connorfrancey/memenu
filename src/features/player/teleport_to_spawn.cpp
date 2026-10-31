#include "core/module.h"
#include "core/notify.h"

namespace {

class TeleportToSpawn final : public ActionModule {
public:
    TeleportToSpawn() : ActionModule("Teleport to Spawn", "Player", "Move the player back to the level start", 40) {}

    void OnTrigger() override
    {
        // TODO: needs the level's PlayerStart (or the checkpoint's location) and a SetLocation on the pawn.
        notify::Push("Teleport to Spawn is not implemented yet");
    }
};
MEM_MODULE(TeleportToSpawn)

} // namespace
