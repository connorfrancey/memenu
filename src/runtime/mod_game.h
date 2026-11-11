#pragma once
// The game side of the mod API (Mirage, step 6): events, function hooks, calls and game objects for MirageApi
// (runtime/mods.cpp puts these functions in the table), and where the menu tells mods what happened.

#include "mirage/api.h"

#include <string>

namespace mirage::mods::game_api
{
	MirageSubscription* MIRAGE_CALL EventSubscribe(MirageMod* mod, int32_t type, MirageEventFn fn, void* user);
	void MIRAGE_CALL EventUnsubscribe(MirageSubscription* subscription);
	MirageHook* MIRAGE_CALL HookFunction(MirageMod* mod, const char* function, int32_t when, int32_t priority,
										 MirageHookFn fn, void* user);
	void MIRAGE_CALL HookRemove(MirageHook* hook);
	MirageObject* MIRAGE_CALL CallObject(MirageCall* call);
	const char* MIRAGE_CALL CallFunction(MirageCall* call);
	void MIRAGE_CALL CallSkip(MirageCall* call);
	int32_t MIRAGE_CALL CallGet(MirageCall* call, const char* path, MirageValue* out);
	int32_t MIRAGE_CALL CallSet(MirageCall* call, const char* path, const MirageValue* in);
	MirageCall* MIRAGE_CALL CallBegin(MirageObject* object, const char* function);
	int32_t MIRAGE_CALL CallInvoke(MirageCall* call);
	void MIRAGE_CALL CallEnd(MirageCall* call);
	MirageObject* MIRAGE_CALL PlayerPawn();
	MirageObject* MIRAGE_CALL PlayerController();
	MirageObject* MIRAGE_CALL WorldInfo();
	MirageObject* MIRAGE_CALL ObjectFind(const char* fullName);
	int32_t MIRAGE_CALL ObjectGet(MirageObject* object, const char* path, MirageValue* out);
	int32_t MIRAGE_CALL ObjectSet(MirageObject* object, const char* path, const MirageValue* in);
	uint32_t MIRAGE_CALL ObjectName(MirageObject* object, char* buffer, uint32_t capacity);
	uint32_t MIRAGE_CALL ObjectClassName(MirageObject* object, char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL ObjectIsA(MirageObject* object, const char* className);
}

namespace mirage::mods
{
	// What happened, to the mods subscribed to it (the menu calls these where the game layer reports it)
	void DispatchTick(float seconds);								// Game thread
	void DispatchLevelLoad(bool finished, const std::string& map);	// Game thread
	void DispatchDeath(bool after);									// Game thread
	void DispatchPlayerState();										// Game thread: reads the move state itself
	void DispatchKey(int vk);										// The window's thread
}
