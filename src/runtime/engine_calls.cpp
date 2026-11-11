#include "runtime/engine_calls.h"

#include "runtime/call.h"

#include "sdk/SDK.hpp"
#include <spdlog/spdlog.h>

#include <set>

namespace mirage::engine
{
	namespace
	{
		// Calls `params` on `object`, logging the first failure of each function (they'd repeat every tick)
		bool Run(Classes::UObject* object, call::Params& params, const char* name, bool setOk = true)
		{
			std::string error;
			if (!params.Valid() || !setOk) error = params.Error();
			else if (call::Call(object, params, &error)) return true;
			static std::set<std::string> reported;
			if (reported.insert(name).second) spdlog::warn("Mirage: {} could not be called: {}", name, error);
			return false;
		}

		Classes::FString Text(const std::wstring& s)
		{
			return Classes::FString(s.c_str());		// Borrows the buffer: the engine copies what it keeps
		}
	}

	bool SetMove(Classes::ATdPawn* pawn, Classes::EMovement move, bool viaReplication, bool checkCanDo)
	{
		call::Params p(pawn, "SetMove");
		const bool ok = p.Set("NewMove", static_cast<std::uint8_t>(move)) && p.Set("bViaReplication", viaReplication) &&
						p.Set("bCheckCanDo", checkCanDo);
		return Run(pawn, p, "SetMove", ok) && p.GetBool("ReturnValue");
	}

	bool SetRotation(Classes::AActor* actor, const Classes::FRotator& rotation)
	{
		call::Params p(actor, "SetRotation");
		const bool ok = p.SetRaw("NewRotation", &rotation, sizeof rotation);
		return Run(actor, p, "SetRotation", ok) && p.GetBool("ReturnValue");
	}

	bool MoveSmooth(Classes::AActor* actor, const Classes::FVector& delta)
	{
		call::Params p(actor, "MoveSmooth");
		const bool ok = p.SetRaw("Delta", &delta, sizeof delta);
		return Run(actor, p, "MoveSmooth", ok) && p.GetBool("ReturnValue");
	}

	bool ToggleReactionTime(Classes::UObject* hud, bool on)
	{
		call::Params p(hud, "ToggleReactionTime");
		return Run(hud, p, "ToggleReactionTime", p.Set("toggle", on));
	}

	bool SetSoundMode(Classes::UObject* controller, std::uint8_t mode, float duration, bool withForce, float fadeTime)
	{
		call::Params p(controller, "SetSoundMode");
		const bool ok = p.Set("SoundMode", mode) && p.Set("Duration", duration) && p.Set("WithForce", withForce) &&
						p.Set("FadeTime", fadeTime);
		return Run(controller, p, "SetSoundMode", ok);
	}

	bool StartGame(Classes::UObject* gameData, const std::wstring& level, const std::wstring& checkpoint,
				   const std::wstring& gameMode, const std::wstring& url, bool saveProgress, bool achievements)
	{
		call::Params p(gameData, "StartGame");
		const Classes::FString texts[] = {Text(level), Text(checkpoint), Text(gameMode), Text(url)};
		const bool ok = p.SetRaw("LevelName", &texts[0], sizeof texts[0]) &&
						p.SetRaw("CheckpointName", &texts[1], sizeof texts[1]) &&
						p.SetRaw("GameMode", &texts[2], sizeof texts[2]) && p.SetRaw("URL", &texts[3], sizeof texts[3]) &&
						p.Set("bShouldSaveCheckpointProgress", saveProgress) && p.Set("bAllowSPLevelAchievements", achievements);
		return Run(gameData, p, "StartGame", ok);
	}

	bool RestartFromLastCheckpoint(Classes::UObject* gameData, bool saveProgress, bool achievements)
	{
		call::Params p(gameData, "RestartFromLastCheckpoint");
		const bool ok = p.Set("bShouldSaveCheckpointProgress", saveProgress) && p.Set("bAllowSPLevelAchievements", achievements);
		return Run(gameData, p, "RestartFromLastCheckpoint", ok);
	}
}
