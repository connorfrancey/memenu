#include "core/module.h"
#include "core/notify.h"
#include "game/game.h"
#include "game/player_state.h"

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

namespace {

// Back to where the level puts the player: the start spot the game itself picks (GameInfo::FindPlayerStart, the
// same choice a respawn makes), facing the way it faces, standing still.
class TeleportToSpawn final : public ActionModule {
public:
    TeleportToSpawn() : ActionModule("Teleport to Spawn", "Player", "Move the player back to the level start", 40) {}

    void OnTrigger() override
    {
        game::RunOnGameThread([] {
            auto* pawn = game::Pawn();
            auto* pc = game::Controller();
            auto* world = game::World();
            if (!pawn || !pc || !world || !world->Game) {
                notify::Push("Teleport to Spawn: no player");
                return;
            }
            Classes::ANavigationPoint* start = world->Game->FindPlayerStart(pc, 0, Classes::FString(L""));
            if (!start) {
                notify::Push("Teleport to Spawn: this level has no start spot");
                return;
            }
            player_state::ResetMovement(pawn, pc);      // Out of any move (wallrun, grab, ...) first
            pawn->Velocity = Classes::FVector{};
            pawn->Acceleration = Classes::FVector{};
            // Written directly, like No Clip and Load State do: the engine's SetLocation() reports success through the
            // SDK but leaves the player where they were.
            pawn->Location = start->Location;
            spdlog::info("Teleport to Spawn: {} at ({:.0f}, {:.0f}, {:.0f})", start->GetName(), start->Location.X,
                         start->Location.Y, start->Location.Z);
            Classes::FRotator rot{};
            rot.Yaw = start->Rotation.Yaw;
            pc->SetRotation(rot);
            pawn->SetRotation(rot);
            notify::Push("Teleported to the level start");
        });
    }
};
MEM_MODULE(TeleportToSpawn)

} // namespace
