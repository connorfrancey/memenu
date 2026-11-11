#pragma once
// Managed native detours (Mirage, step 6): mods detour the game's native code through the runtime, which owns the one
// MinHook hook per target and chains the mods' detours on it in priority order.
//
//     static int (__thiscall* original)(void* self, int x) = nullptr;
//     int __fastcall Mine(void* self, void*, int x) { return original(self, x) + 1; }
//     detours::Add(target, &Mine, 10, reinterpret_cast<void**>(&original), "my-mod", &error);
//
// A chain on one target runs from the highest priority down: each detour's `*original` is the next detour, the last
// one's is the real function. When the chain changes (a detour added or removed), every `*original` is set again, so a
// mod keeps calling its `original` as before. Two mods on one target is logged (who, in what order); a target the menu
// hooks itself is refused.
//
// A detour that is removed while another thread runs inside it (the game calling the function at that moment) is not
// waited for: remove detours when the code they hook isn't running (a mod's unload, which waits for its other calls).

#include <string>
#include <vector>

namespace mirage::detours
{
	// `owner` names who it is for in messages ("hello"). Returns its id (> 0), or 0 and `error`.
	int Add(void* target, void* detour, int priority, void** original, const std::string& owner, std::string* error = nullptr);
	void Remove(int id);
	void RemoveAll();

	// The chain on `target`, first to last: "owner (priority)" each. Empty if none.
	std::vector<std::string> Chain(void* target);
}
