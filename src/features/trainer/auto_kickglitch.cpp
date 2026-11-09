#include "features/trainer/auto_kickglitch.h"

#include "core/module.h"
#include "game/flight.h"
#include "game/game.h"

#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <mutex>

namespace {

using Classes::EMovement;
using Classes::EPhysics;

constexpr const char* kAttackPress = "Function TdGame.TdPlayerController.AttackPress";
constexpr const char* kAttackRelease = "Function TdGame.TdPlayerController.AttackRelease";
constexpr int kWallKickTries = 8;       // Ticks of wallrunning to get the kick in (the wallrun may not take it at once)

// ---- The kick buffer (game thread: the script event filters and OnTick) ------------------------------------------------
// A kick pressed in the air is held back (AttackPress never reaches the game, nor its release). When the player is
// wallrunning within the window, it is pressed for them on the first tick they can kick. Otherwise it is let go late, or
// dropped.
struct Buffer {
    bool on = false;            // The module is on with the buffer enabled (set every tick)
    bool injecting = false;     // Our own AttackPress/Release: let through
    bool held = false;          // A kick is waiting for a wall
    float since = 0.f;          // Game time it was pressed (WorldInfo.TimeSeconds: a TAS's clock, so it replays the same)
    int wallTicks = 0;          // Ticks spent trying to kick off the wall
};
Buffer g_buffer;

std::mutex g_statsMutex;
auto_kickglitch::BufferStats g_stats;

template <class F>
void Count(F&& f)
{
    std::lock_guard lock(g_statsMutex);
    f(g_stats);
}

float GameTime()
{
    auto* world = game::World();
    return world ? world->TimeSeconds : 0.f;
}

bool FilterPress(Classes::UObject* self, void*)
{
    if (!g_buffer.on || g_buffer.injecting || self != game::Controller()) return false;
    auto* pawn = game::Pawn();
    if (!pawn || pawn->Health <= 0 || pawn->Weapon) return false;      // With a gun the press shoots: never held
    if (pawn->Physics.GetValue() != EPhysics::PHYS_Falling) return false;   // On the ground or a wall: kick now
    if (g_buffer.held) return true;     // Pressed again while waiting: still the one kick
    g_buffer.held = true;
    g_buffer.since = GameTime();
    g_buffer.wallTicks = 0;
    Count([](auto& s) { ++s.held; });
    return true;
}

bool FilterRelease(Classes::UObject* self, void*)
{
    // The press was held back: so is its release (it is sent with the kick)
    return g_buffer.on && !g_buffer.injecting && g_buffer.held && self == game::Controller();
}

void Kick(Classes::ATdPlayerController* controller)
{
    g_buffer.injecting = true;
    controller->AttackPress();
    controller->AttackRelease();
    g_buffer.injecting = false;
}

// The Kickglitch tool, always on: every wallrun kick you do gets its jump on the frame you land, like the tool's (the
// first tick the physics is walking again). The kick stays yours, unless "Kick key" is held: then it kicks too, as
// soon as you are wallrunning. With the kick buffer, a kick pressed just before the wall waits for it.
class AutoKickglitch final : public Module {
public:
    AutoKickglitch()
        : Module("Auto Kickglitch", "Trainer", "Jumps on the landing frame after every wallrun kick you do", 12)
    {
        schedule.priority = sched::Priority::Critical;     // The landing frame is one tick: never deferred
        bufferWindow.VisibleIf([this] { return buffer.Get(); });
        onMiss.VisibleIf([this] { return buffer.Get(); });
    }

    void OnDisable() override
    {
        game::RunOnGameThread([this] {
            kicked_ = false;
            g_buffer.on = false;
            g_buffer.held = false;
        });
    }

    void OnTick() override
    {
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        if (buffer.Get()) Hook();
        g_buffer.on = buffer.Get() && hooked_;
        if (!pawn || !controller || !controller->PlayerInput || pawn->Health <= 0 || flight::Active()) {
            kicked_ = false;
            g_buffer.held = false;
            return;
        }
        const auto move = pawn->MovementState.GetValue();
        const auto physics = pawn->Physics.GetValue();

        if (physics == EPhysics::PHYS_WallRunning && kickKey.Get() && kickKey.Down() && !kicked_) Kick(controller);
        TickBuffer(controller, move, physics);

        if (move == EMovement::MOVE_MeleeWallrun) {
            if (!kicked_) {
                kicked_ = true;
                kickTicks_ = 0;
            }
        }
        if (!kicked_) return;

        if (physics == EPhysics::PHYS_Walking) {
            controller->PlayerInput->Jump();
            kicked_ = false;
        } else if (++kickTicks_ > 240 || IsOtherMove(move)) {
            kicked_ = false;        // Grabbed a ledge, wallran again, ...: no landing to jump from
        }
    }

private:
    // The filters on the game's kick press and release (game thread, game ready: from OnTick). Watches can't be
    // removed, so once for good; they do nothing unless g_buffer.on. A failure is tried again now and then.
    void Hook()
    {
        if (hooked_ || (retryIn_ > 0 && --retryIn_ > 0)) return;
        static bool press = false, release = false;
        if (!press) press = game::BlockScriptEvent(kAttackPress, &FilterPress);
        if (!release) release = game::BlockScriptEvent(kAttackRelease, &FilterRelease);
        hooked_ = press && release;
        if (!hooked_) {
            retryIn_ = 300;
            spdlog::warn("Auto Kickglitch: the kick can't be held back yet (no kick buffer); trying again");
        }
        Count([this](auto& s) { s.hooked = hooked_; });
    }

    // A held kick: off the wall as soon as the player can, else late or dropped when the window runs out.
    void TickBuffer(Classes::ATdPlayerController* controller, EMovement move, EPhysics physics)
    {
        if (!g_buffer.held) return;
        if (move == EMovement::MOVE_MeleeWallrun) {         // Kicking off the wall: done
            g_buffer.held = false;
            Count([](auto& s) { ++s.onWall; });
            return;
        }
        if (physics == EPhysics::PHYS_WallRunning) {
            if (g_buffer.wallTicks++ < kWallKickTries) Kick(controller);    // Until the wallrun takes it
            else Drop();
            return;
        }
        const bool expired = GameTime() - g_buffer.since > bufferWindow.Get() / 1000.f;
        if (physics == EPhysics::PHYS_Falling && !expired) return;          // Still on its way to the wall
        if (physics != EPhysics::PHYS_Falling || onMiss.Get() == 1) {
            Drop();                 // Landed, grabbed a ledge, ...: that kick is not wanted any more
            return;
        }
        g_buffer.held = false;      // No wall came: the air kick it would have been, late
        Kick(controller);
        Count([](auto& s) { ++s.late; });
    }

    static void Drop()
    {
        g_buffer.held = false;
        Count([](auto& s) { ++s.dropped; });
    }

    static bool IsOtherMove(EMovement m)
    {
        switch (m) {
            case EMovement::MOVE_Grabbing:
            case EMovement::MOVE_WallRunningLeft:
            case EMovement::MOVE_WallRunningRight:
            case EMovement::MOVE_WallClimbing:
            case EMovement::MOVE_IntoGrab:
            case EMovement::MOVE_ZipLine:
            case EMovement::MOVE_FallingUncontrolled:
            case EMovement::MOVE_Jump:
            case EMovement::MOVE_WallRunJump:
                return true;
            default:
                return false;
        }
    }

    // Game thread
    bool hooked_ = false;           // The kick press and release are watched
    int retryIn_ = 0;               // Ticks before hooking is tried again
    bool kicked_ = false;           // A kick was seen: jump on its landing
    int kickTicks_ = 0;

    ctl::KeyHold kickKey{this, "Kick key", 0, "Held while wallrunning: kicks off the wall for you too (none: you kick)"};
    ctl::Checkbox buffer{this, "Kick buffer", true,
                         "Kick pressed in the air, just before the wall: instead of an air kick it waits, and kicks off "
                         "the wall on the first frame you are on it. Not with a gun (that shoots)"};
    ctl::SliderInt bufferWindow{this, "Buffer window", 200, 50, 600, "%d ms", 10,
                                "How long a held kick waits for a wall (game time)"};
    ctl::Combo onMiss{this, "No wall in time", {"Kick anyway", "Drop it"}, 0,
                      "When the window runs out in the air. Kick anyway: the air kick, late. Drop it: no kick. "
                      "Landing or grabbing a ledge first always drops it"};
};
MEM_MODULE(AutoKickglitch)

} // namespace

auto_kickglitch::BufferStats auto_kickglitch::Stats()
{
    std::lock_guard lock(g_statsMutex);
    return g_stats;
}
