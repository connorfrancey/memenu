#pragma once

// Free movement for the local player: "No Clip" ignores all collision, "Flight" keeps colliding with the world.
// Everything here runs on the game thread (call Tick from a module's onTick, Stop through game::RunOnGameThread).
namespace flight
{
    enum class Mode { Flight, NoClip };

    struct Settings
    {
        float speed = 900.f;          // units per second
        float boost = 3.f;            // speed multiplier while the boost key is held
        float responsiveness = 10.f;  // how fast the velocity follows the input (higher = snappier)
        bool followView = true;       // forward follows the camera pitch, otherwise movement stays level
        bool invulnerable = true;     // keep health full and prevent fall damage while active
        bool inputEnabled = true;     // false (menu open): no input, the player just hovers
        bool blockActions = true;     // ignore jump/crouch/etc. while active (the up/down keys are often those)
        bool up = false;              // vertical keys, polled by the caller
        bool down = false;
        bool boostHeld = false;
    };

    // Moves the player for this tick (the time step is measured internally), starting the mode on the first call.
    // Safe to call without a player: it waits.
    void Tick(Mode mode, const Settings& settings);

    // Hands the player back to the normal physics (keeping the current velocity). A no-op unless `mode` is active.
    void Stop(Mode mode);

    bool Active();
}
