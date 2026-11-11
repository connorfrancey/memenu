#pragma once
// UnrealScript function hooks (Mirage, step 6): run code before or after a function of the game, or instead of it,
// with its parameters by name.
//
// How: every script function runs through UObject::ProcessInternal, with a new frame whose Locals hold the evaluated
// parameters: the VM calls it directly for a call from script, and through UFunction::Func for one from native code or
// ProcessEvent. ProcessInternal is detoured once, at the first hook; it looks the frame's function up in a small
// lock-free table and runs the hooks around the original. A function nobody hooks costs a hash and a load.
//
// The offsets this needs (UFunction::Func, which holds ProcessInternal's address; FFrame::Node and Locals) aren't in
// any package: they are found in the game and checked, or hooks stay off.
//
// Native functions (FUNC_Native) get a thunk in their UFunction::Func instead. Their code reads its parameters from the
// caller's bytecode, so they are hooked before and after (to see that they ran, and on what) without parameters and
// can't be replaced. One with a native index (iNative) is called by the VM straight from the bytecode, never through
// Func: only calls through ProcessEvent reach its hooks. Its code can be detoured instead (managed detours).
//
// Game thread for the calls; Add / Remove from any thread.

#include "runtime/reflect.h"

#include <cstdint>
#include <string>

namespace Classes
{
	class UObject;
	class UFunction;
}

namespace mirage::hooks
{
	enum class When : std::uint8_t { Before, After, Replace };

	// What a hook sees: one call of the hooked function
	struct Call
	{
		Classes::UObject* object = nullptr;		// The object it is called on
		Classes::UFunction* function = nullptr;
		std::uint8_t* params = nullptr;			// Its parameters and locals (nullptr for a native function called from script)
		void* result = nullptr;					// Where its return value goes
		bool skip = false;						// Before: don't run the function (a script function only)
		bool native = false;

		// A parameter ("NewLocation", "NewLocation.Z") or "ReturnValue": where it is, to read and write with reflect::
		reflect::Value Param(std::string_view path) const;
	};

	using Callback = void (*)(void* user, Call& call);

	// `function`: its path ("Engine.Actor.SetLocation", "TdGame.TdPawn.Walking.EndState" for a state's). Hooks of a
	// function run by priority, higher first (then in the order they were added); Replace hooks are Before hooks that
	// skip the function (the first one to run decides the return value). Returns the hook's id (> 0), 0 and `error`
	// when it can't be hooked. A function that isn't loaded yet (a level's) is hooked when it shows up.
	int Add(const std::string& function, When when, int priority, Callback callback, void* user, std::string* error = nullptr);
	// The function goes back to its original entry point when its last hook is removed. Any thread; a call running on
	// the game thread finishes with the hooks it started with.
	void Remove(int id);

	// Looks for functions not found yet (after a level load). Game thread.
	void Retry();
	// Every hook off and every function back as it was, ProcessInternal's detour removed: before the menu unloads
	void RemoveAll();

	// Whether hooks work in this game (the offsets were found), and if not, why
	bool Ready(std::string* why = nullptr);

	struct Stats
	{
		std::uint64_t calls = 0;			// Hooked calls run
		double microseconds = 0.0;			// Spent in the thunk around the original, total (hooks included)
		int functions = 0, hooks = 0;		// Hooked now
	};
	Stats GetStats();
}
