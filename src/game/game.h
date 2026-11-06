#pragma once
#include <cstddef>
#include <functional>
#include <string>

#include "render/math.h"

// Thin layer between this project and the mmultiplayer Mirror's Edge SDK (Client/sdk).
// Everything that touches game memory goes through here, so modules never deal with GNames/GObjects,
// hooks or threads themselves.
//
// Threading: the menu (ImGui) runs on the render thread, the game on its own thread. Game objects must
// only be touched from the game thread: do it from a tick callback (modules::Tick is registered as one)
// or queue work with RunOnGameThread().

namespace Classes {
	class UObject;
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
		unsigned long long objectScans = 0; // Times the player lookup walked the whole object list
		unsigned deaths = 0;                // Player deaths seen by the death hook
		unsigned long long scriptEvents = 0;    // Watched script functions that ran
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
	
	float EngineNear();					// Near clip plane of the engine's projection (0 = unknown; always, for now)

	bool Ready();                       // SDK globals resolved and hooks installed
	bool IsLoading();                   // A level is being loaded

	void OnTick(TickCallback callback); // Runs on the game thread, once per engine tick, not while loading
	void RunOnGameThread(std::function<void()> fn);   // Runs on the next tick

	// The local player's move state handler, on the tick its state has just changed (the hook mmultiplayer's trainer uses
	// for its "strang" tool). Game thread. Return true to skip the engine's handler for this call.
	using StateChangeCallback = bool(*)();
	void OnPlayerStateChange(StateChangeCallback callback);
	bool PlayerStateHooked();           // The handler was found and hooked (false: the callbacks never run)

	// The player died: `after` false just before the game handles the death, true right after. Game thread.
	using DeathCallback = void (*)(bool after);
	void OnPlayerDeath(DeathCallback callback);
	bool DeathHooked();

	// A level load starts (finished = false) and ends (finished = true). Game thread, inside the engine's load function.
	using LevelLoadCallback = void (*)(bool finished);
	void OnLevelLoad(LevelLoadCallback callback);
	
	// A script function the engine calls from native code through UObject::ProcessEvent: events like a Kismet action's
	// Activated(), delegates of the UI. `fullName` as the engine names it ("Function TdGame.SeqAct_TdLevelCompleted.Activated").
	// The callback runs on the game thread before the function itself, with the object it is called on and its parameters.
	// ProcessEvent is only hooked once something asks for an event. Up to 64 watches (a function may have several);
	// returns false if the hook failed. `after`: run once the function has returned (its out parameters are set).
	using ScriptEventCallback = void (*)(Classes::UObject* self, void* params);
	bool OnScriptEvent(const char* fullName, ScriptEventCallback callback, bool after = false);
	// Before the function: return true and the engine doesn't run it (nor the other watches' after-callbacks).
	using ScriptEventFilter = bool (*)(Classes::UObject* self, void* params);
	bool BlockScriptEvent(const char* fullName, ScriptEventFilter filter);
	// Hooked and every watched function found in the game (they are looked up on the game thread, after a tick).
	bool ScriptEventsReady();

	// The world asked the engine to finish its streaming loads before the next frame: the game freezes on a loading
	// icon for a moment (elevators, doors). Game thread.
	bool BlockingLoad();
	// Package name of the level the player is in ("edge_p"), as the game spells it. Empty while loading. Game thread.
	std::string MapName();

	// Runs a console command as if typed in the game's console ("set Engine bEnableColorClear true", "open edge_p",
	// "stat fps"). Game thread. False without a player controller to run it.
	bool ConsoleCommand(const std::string& command);

	// Byte pattern scan over MirrorsEdge.exe ('x' = must match, '?' = any byte). nullptr if not found.
	void* FindPattern(const char* pattern, const char* mask);
	// The same within [start, start + size) of the executable's image.
	void* FindPatternIn(const void* start, std::size_t size, const char* pattern, const char* mask);
	
	// Null while loading or outside a level. Game thread only.
	Classes::ATdPlayerController* Controller();
	Classes::ATdPlayerPawn*       Pawn();
	Classes::AWorldInfo*          World();
}