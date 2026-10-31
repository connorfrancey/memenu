#include "flight.h"
#include "flight_math.h"
#include "game.h"

#include <Windows.h>

#include <mirrorsedge_sdk.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace flight
{
    namespace
    {
        struct State
        {
            bool active = false;
            Mode mode = Mode::NoClip;
            Vec velocity;                 // units per second
            bool hasLast = false;         // timing of the previous tick
            std::chrono::steady_clock::time_point last;
            unsigned char prevIgnoreButtons = 0;
        };

        State g_state; // Game thread only

        const char* Name(Mode m) { return m == Mode::NoClip ? "No Clip" : "Flight"; }

        // Same cleanup mmultiplayer's trainer does before it starts flying: stop slides/animations and leave any
        // special movement so the pawn is in a plain state.
        void Start(Classes::ATdPlayerPawn* pawn, Classes::ATdPlayerController* controller, Mode mode)
        {
            pawn->StopSlideEffect();
            pawn->StopAllCustomAnimations(0.0f);
            pawn->SetMove(Classes::EMovement::MOVE_Walking, false, false);

            g_state.active = true;
            g_state.mode = mode;
            g_state.velocity = {};
            g_state.hasLast = false;
            g_state.prevIgnoreButtons = controller->bIgnoreButtonInput;
            spdlog::info("{}: started at ({:.0f}, {:.0f}, {:.0f})", Name(mode), pawn->Location.X, pawn->Location.Y,
                         pawn->Location.Z);
        }

        // Back to normal physics, keeping the speed we had so leaving flight feels continuous.
        void Restore(Classes::ATdPlayerPawn* pawn, Classes::ATdPlayerController* controller)
        {
            if (controller) controller->bIgnoreButtonInput = g_state.prevIgnoreButtons;

            Classes::FVector v{};
            v.X = g_state.velocity.x;
            v.Y = g_state.velocity.y;
            v.Z = g_state.velocity.z;
            pawn->Velocity = v;
            pawn->bCollideWorld = true;
            pawn->EnterFallingHeight = -1e30f; // no fall damage for the distance we flew
            pawn->Physics = Classes::EPhysics::PHYS_Falling;

            spdlog::info("{}: stopped at ({:.0f}, {:.0f}, {:.0f}), velocity ({:.0f}, {:.0f}, {:.0f})", Name(g_state.mode),
                         pawn->Location.X, pawn->Location.Y, pawn->Location.Z, g_state.velocity.x, g_state.velocity.y,
                         g_state.velocity.z);
            g_state.active = false;
            g_state.velocity = {};
        }
    }

    bool Active() { return g_state.active; }

    void Tick(Mode mode, const Settings& s)
    {
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        if (!pawn || !controller)
        {
            if (g_state.active)
            {
                spdlog::debug("{}: no player right now (level change?), will restart when one appears", Name(g_state.mode));
                g_state.active = false;
            }
            return;
        }
        if (pawn->Health <= 0)
        {
            if (g_state.active)
            {
                spdlog::info("{}: player died, stopping", Name(g_state.mode));
                g_state.active = false;
            }
            return;
        }

        if (g_state.active && g_state.mode != mode) Restore(pawn, controller); // switched between the two modules
        if (!g_state.active) Start(pawn, controller, mode);

        // Time step: measured here so callers do not have to pass one. Clamped so a hitch cannot launch the player.
        const auto now = std::chrono::steady_clock::now();
        float dt = g_state.hasLast ? std::chrono::duration<float>(now - g_state.last).count() : 0.f;
        g_state.last = now;
        g_state.hasLast = true;
        dt = std::clamp(dt, 0.f, 0.1f);

        // ---- input -----------------------------------------------------------------------------------------------
        float forward = 0.f, right = 0.f, vertical = 0.f;
        if (s.inputEnabled)
        {
            if (controller->PlayerInput)
            {
                forward = controller->PlayerInput->RawJoyUp;
                right = controller->PlayerInput->RawJoyRight;
            }
            vertical = (s.up ? 1.f : 0.f) - (s.down ? 1.f : 0.f);
        }

        const Vec dir = Direction(controller->Rotation.Yaw, controller->Rotation.Pitch, forward, right, vertical,
                                  s.followView);
        const float speed = s.speed * (s.boostHeld ? s.boost : 1.f);

        g_state.velocity.x = Approach(g_state.velocity.x, dir.x * speed, s.responsiveness, dt);
        g_state.velocity.y = Approach(g_state.velocity.y, dir.y * speed, s.responsiveness, dt);
        g_state.velocity.z = Approach(g_state.velocity.z, dir.z * speed, s.responsiveness, dt);

        // ---- apply -----------------------------------------------------------------------------------------------
        // The game's own physics must not fight us: no gravity, no leftover velocity.
        pawn->Physics = Classes::EPhysics::PHYS_None;
        pawn->Velocity = Classes::FVector{};
        if (s.blockActions) controller->bIgnoreButtonInput = 1;

        if (s.invulnerable)
        {
            pawn->Health = pawn->MaxHealth;
            pawn->EnterFallingHeight = -1e30f;
        }

        // Hands snap to nearby surfaces: keep the IK off so they don't stick to geometry while we move through it.
        if (pawn->LeftHandWorldIKController) pawn->LeftHandWorldIKController->StrengthTarget = 0.0f;
        if (pawn->RightHandWorldIKController) pawn->RightHandWorldIKController->StrengthTarget = 0.0f;

        const float dx = g_state.velocity.x * dt, dy = g_state.velocity.y * dt, dz = g_state.velocity.z * dt;

        if (mode == Mode::NoClip)
        {
            // Teleport-style movement (as the trainer does): never tests collision, so walls and floors don't matter.
            pawn->bCollideWorld = false;
            pawn->Location.X += dx;
            pawn->Location.Y += dy;
            pawn->Location.Z += dz;
        }
        else if (dx != 0.f || dy != 0.f || dz != 0.f)
        {
            // Engine move with collision and sliding along surfaces.
            pawn->bCollideWorld = true;
            Classes::FVector delta{};
            delta.X = dx;
            delta.Y = dy;
            delta.Z = dz;
            pawn->MoveSmooth(delta);
        }
    }

    void Stop(Mode mode)
    {
        if (!g_state.active || g_state.mode != mode) return;
        if (auto* pawn = game::Pawn())
            Restore(pawn, game::Controller());
        else
        {
            spdlog::debug("{}: stop requested without a player", Name(mode));
            g_state.active = false;
        }
    }
}
