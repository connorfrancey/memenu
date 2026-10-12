#pragma once
#include <functional>

// Thin layer between this project and the mmultiplayer Mirror's Edge SDK (Client/sdk).
// Everything that touches game memory goes through here, so modules never deal with GNames/GObjects,
// hooks or threads themselves.
//
// Threading: the menu (ImGui) runs on the render thread, the game on its own thread. Game objects must
// only be touched from the game thread: do it from a tick callback (modules::Tick is registered as one)
// or queue work with RunOnGameThread().

namespace Classes {
    class AWorldInfo;
    class ATdPlayerController;
    class ATdPlayerPawn;
}

namespace game {

    using TickCallback = void (*)(float deltaSeconds);

    // Call after hooks::Install() (MinHook must be initialised). Finds GNames/GObjects by pattern, which also
    // pins this to the game version the patterns were made for, and hooks the engine tick and level loading.
    // Returns false if a pattern was not found: the menu still works, game modules just can't do anything.
    bool Init();

    // Waits (briefly) for queued game-thread work to run. Call before hooks::Uninstall().
    void Shutdown();

    // Snapshot for diagnostics, safe to read from any thread (updated by the game thread).
    struct Status
    {
        bool ready = false;     // Patterns found, hooks installed
        bool loading = false;
        bool hasPawn = false;
        int health = 0, maxHealth = 0;
        unsigned long long ticks = 0;       // Engine ticks seen: 0 means the Tick hook never fires
    };
    Status GetStatus();

    // While blocked, the local player's look input is ignored (menu open). Restored when unblocked and on Shutdown().
    // This also covers gamepad sticks; mouse/keyboard messages are filtered separately in hooks.cpp.
    void SetInputBlocked(bool blocked);

    bool Ready();                       // SDK globals resolved and hooks installed
    bool IsLoading();                   // A level is being loaded

    void OnTick(TickCallback callback); // Runs on the game thread, once per engine tick, not while loading
    void RunOnGameThread(std::function<void()> fn);   // Runs on the next tick

    // Null while loading or outside a level. Game thread only.
    Classes::ATdPlayerController* Controller();
    Classes::ATdPlayerPawn*       Pawn();
    Classes::AWorldInfo*          World();

}