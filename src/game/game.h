#pragma once
#include <functional>

#include "render/math.h"

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
		bool ready = false;                 // Patterns found, hooks installed
		bool loading = false;
		bool hasPawn = false;               // Local TdPlayerPawn found
		int health = 0, maxHealth = 0;
		unsigned long long ticks = 0;       // Engine ticks seen: 0 means the Tick hook never fires
	};
	Status GetStatus();

	// While blocked, the local player's look input is ignored (menu open). Restored when unblocked and on Shutdown().
	// This also covers gamepad sticks; mouse/keyboard messages are filtered separately in hooks.cpp.
	void SetInputBlocked(bool blocked);
	// A TAS session owns all input: while suspended the menu doesn't set bIgnoreLookInput (that would change the run).
	void SetInputBlockedSuspended(bool suspended);

	// Latest camera of the local player (what the world is rendered from). Only tracked while enabled: it costs a lookup per tick.
	void SetCameraTracking(bool enabled);
	rmath::Camera GetCamera();			// Any thread; valid == false when there is no camera yet
	
	float EngineNear();					// Near clip plane of the engine's projection (0 until known)

	bool Ready();                       // SDK globals resolved and hooks installed
	bool IsLoading();                   // A level is being loaded

	void OnTick(TickCallback callback); // Runs on the game thread, once per engine tick, not while loading
	void RunOnGameThread(std::function<void()> fn);   // Runs on the next tick
	
	
	// The local player's move state handler just switched to a new state (the hook mmultiplayer's trainer uses for its
	// "strang" tool). Game thread. Return true to skip the engine's handler for this call.
	using StateChangeCallback = bool(*)();
	void OnPlayerStateChange(StateChangeCallback callback);
	
	// A level load starts (finished = false) and ends (finished = true). Game thread, inside the engine's load function.
	using LevelLoadCallback = void (*)(bool finished);
	void OnLevelLoad(LevelLoadCallback callback);
	
	// Byte pattern scan over MirrorsEdge.exe ('x' = must match, '?' = any byte). nullptr if not found.
	void* FindPattern(const char* pattern, const char* mask);
	void RunOnGameThread(std::function<void()> fn);   // Runs on the next tick
	
	// Null while loading or outside a level. Game thread only.
	Classes::ATdPlayerController* Controller();
	Classes::ATdPlayerPawn*       Pawn();
	Classes::AWorldInfo*          World();
}