#pragma once

// Save states for the local player, like mmultiplayer's trainer: a snapshot of every plain value (bool, byte, int,
// float, vector, rotator) of the pawn, its controller and each of the pawn's moves, put back on load. Object pointers,
// names, strings and arrays are never touched, so a load can't leave a dangling reference behind.
//
// The trainer copies those values with ~2800 lines of generated code. Here they are found through the engine's own
// property list (UClass -> UProperty), whose layout the SDK doesn't describe: the offsets inside UProperty are found
// once at runtime by matching a few properties the SDK does know (Location, Health, ...). If that fails, Save() says so
// and nothing is ever written.
//
// Game thread only.

namespace Classes 
{
	class ATdPlayerController;
	class ATdPlayerPawn;
}

namespace player_state 
{
	bool Save();			// False: no player, or the property layout could not be worked out (see the log)
	bool Load();			// False: nothing saved, no player, or the player can't be moved right now
	bool HasSave();
	void Clear();
	unsigned LoadCount();	// Bumped by every successful Load (the checkpoint timer restarts on it)
	
	// Puts the player back into plain walking: clears a death fall, a slide, a stuck animation and blocked input. What the
	// trainer does before God/Fly take over from a "falling uncontrolled" state.
	void ResetMovement(Classes::ATdPlayerPawn* pawn, Classes::ATdPlayerController* controller);
}