#pragma once
#include <cstdint>
#include <string>

namespace Classes
{
	class UObject;
	class AActor;
	class ATdPawn;
	struct FRotator;
	struct FVector;
	enum class EMovement : std::uint8_t;
}

// The game functions the menu calls, through mirage::call (the engine's own parameter layout), instead of the SDK's
// wrappers. Those were wrong for each of these: SetMove's and the others' bool parameters were 1 byte where the engine
// reads 4 (their flags came from stack garbage, SetMove's return value was written past its struct), and SetRotation
// and MoveSmooth have native indexes, which ProcessEvent refuses (they never ran).
//
// Game thread. False when the function couldn't be called (logged once per function).
namespace mirage::engine
{
	bool SetMove(Classes::ATdPawn* pawn, Classes::EMovement move, bool viaReplication = false, bool checkCanDo = false);
	bool SetRotation(Classes::AActor* actor, const Classes::FRotator& rotation);
	bool MoveSmooth(Classes::AActor* actor, const Classes::FVector& delta);
	bool ToggleReactionTime(Classes::UObject* hud, bool on);
	bool SetSoundMode(Classes::UObject* controller, std::uint8_t mode, float duration, bool withForce, float fadeTime);
	// TdGameData (the game's data store): what its own level select and pause menu call
	bool StartGame(Classes::UObject* gameData, const std::wstring& level, const std::wstring& checkpoint,
				   const std::wstring& gameMode, const std::wstring& url, bool saveProgress, bool achievements);
	bool RestartFromLastCheckpoint(Classes::UObject* gameData, bool saveProgress, bool achievements);
}
