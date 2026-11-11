#pragma once
// Inside the mod runtime (runtime/mods.cpp, runtime/mod_game.cpp): the record of a mod, and calling into its code.

#include "mirage/api.h"
#include "runtime/manifest.h"
#include "runtime/mod_lua.h"

#include <toml.hpp>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Module;
namespace ctl
{
	class Setting;
}

namespace mirage::mods
{
	class ModModule;
	enum class State : int { NotLoaded, Loading, Active, Detached, Failed, Crashed };
	struct HookRecord;
	struct Subscription;
}
namespace mirage::content
{
	class Set;
}

// The runtime's record of a mod: what api.h calls MirageMod. Lives until the menu unloads (its modules point at it).
struct MirageMod
{
	mirage::mods::Manifest manifest;
	std::string id, version, dir;						// Kept as C strings for the API
	std::atomic<mirage::mods::State> state{mirage::mods::State::NotLoaded};
	std::atomic<int> inflight{0};						// Calls into its code running right now (any thread)
	HMODULE dll = nullptr;
	std::filesystem::path shadow;						// The copy of its DLL that is loaded (hot reload), or empty
	MirageModUnloadFn unload = nullptr;
	mirage::mods::LuaModPtr lua;
	std::vector<mirage::mods::ModModule*> modules;		// Every module it ever registered, in order (the menu owns them)
	std::vector<std::unique_ptr<Module>> pending;		// Registered by the current load, not in the menu yet
	std::map<std::string, ctl::Setting*> declared;		// Its mod.toml settings that were made, by key
	std::mutex configLock;
	toml::value config;
	bool configRead = false;
	std::uintmax_t fingerprint = 0;						// Of its files, at the last load (hot reload)
	std::uintmax_t seen = 0;							// At the last check: a reload waits until they stop changing
	bool warnedNoReload = false;
	int loadIndex = -1, reloads = 0;
	std::string problem;
	std::vector<std::string> warnings;
	// Its function hooks and event subscriptions (step 6). Records are kept until the menu unloads: a call that started
	// before a hook was removed may still hold one.
	std::vector<std::unique_ptr<mirage::mods::HookRecord>> hooks;
	std::vector<std::shared_ptr<mirage::mods::Subscription>> subscriptions;
	std::vector<int> detours;							// mirage::detours ids, removed with its hooks
	// Its content (step 7): what its content folders change, and the module that turns it on and off
	std::shared_ptr<mirage::content::Set> content;
	Module* contentModule = nullptr;
};

namespace mirage::mods
{
	// A mod's function hook: the runtime's callback runs it guarded, while the mod runs
	struct HookRecord
	{
		MirageMod* mod = nullptr;
		MirageHookFn fn = nullptr;
		void* user = nullptr;
		std::string function;
		int id = 0;								// mirage::hooks id (0: removed)
	};
	// A mod's event subscription
	struct Subscription
	{
		MirageMod* mod = nullptr;
		int32_t type = 0;
		MirageEventFn fn = nullptr;
		void* user = nullptr;
		std::atomic<bool> active{true};
	};
}

namespace mirage::mods::detail
{
	// Its code may run: loading or loaded, not stopped
	bool Running(const MirageMod& mod);
	// Runs `fn(arg)`, which calls into `mod`'s code, the way every call into a mod runs: only while it is running,
	// counted (a reload waits for it), guarded (a crash stops the mod, names it with `what` and `where`, and records
	// it). False when it didn't run or crashed.
	bool Enter(MirageMod& mod, const std::string& what, const std::string& where, void (*fn)(void*), void* arg);
	// Its hooks and subscriptions off (unload, reload, crash): mod_game.cpp
	void RemoveGameHooks(MirageMod& mod);
	void ReleaseScripts(MirageMod& mod);	// Its script patches undone, its open scripts closed (mod_script.cpp)
}

