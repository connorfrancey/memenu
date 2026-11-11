#pragma once
// Mirage mods (steps 4 and 5): the runtime's side of api/mirage/api.h, and the mods' lifecycle.
//
// Each mod is a folder with a mod.toml (runtime/manifest.h), and a DLL (entry) and/or Lua (main.lua). Its modules are
// ordinary menu modules (Module subclasses): they show in the menu, save their settings in the menu's config and run
// through the scheduler like the built-in ones.
//
// Lifecycle:
//   Discover   reads every mod.toml in the folders and decides what loads, in what order (runtime/resolve.h). A mod
//              that crashed the last time (an entry in <folder>/crashes.toml) is left out, with the crash as reason.
//   LoadPhase  loads the mods of a phase, in that order: early at the menu's start, engine once the game layer is up,
//              world when the first level has loaded. Their modules join the menu as a batch when the load is done.
//   Reload     hot reload: the old code's modules are switched off (its on_disable undoes its changes), the code is
//              unloaded and loaded again, and the modules it registers again are the same menu modules: their settings
//              and on/off state carry over. Lua mods reload when their files change; native ones when mod.toml says
//              hot_reload = true (their DLL runs from a copy, so the build can replace the original).
//   a crash    in a mod's code (caught around every call into it) stops the mod, names it in the log with the
//              exception and the place in its DLL, and is written to crashes.toml so it isn't loaded next time.
//   UnloadAll  in reverse load order, at the menu's unload.

#include "mirage/api.h"
#include "runtime/manifest.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mirage::mods
{
	inline constexpr const char* kRuntimeVersion = "0.1.0";

	// The API table every mod gets (Lua mods too, through runtime/mod_lua.cpp)
	const MirageApi* Api();

	// Reads the mods in `folders` (each subfolder with a mod.toml) and resolves the load order. Once, at the start.
	void Discover(const std::vector<std::filesystem::path>& folders);
	// Loads the discovered mods of `phase`. Early: from the menu's thread, after modules::Init and before config::Load
	// and sched::Init. Engine and world: once modules::Start has run (their modules are published to the menu).
	void LoadPhase(Phase phase);
	// Unloads every mod, last loaded first: after modules::DisableAll and the hooks are gone
	void UnloadAll();

	// Hot reload of a loaded mod. Game thread (between ticks: no on_tick is running). "" when it reloaded, else why not.
	std::string Reload(const std::string& id);
	// Reloads the mods whose files changed. Game thread; runs as a scheduler task every half second.
	void ReloadChanged();

	// A crash outside the guarded calls (a thread a mod started): the mod whose DLL holds `address`, "" if none. Writes
	// the crash record for it. Any thread, from the crash handler (which sees exceptions first: one the mod's own
	// __try handles on a thread it started would be blamed too; the calls the runtime guards are told apart).
	std::string AttributeCrash(const void* address, unsigned long code);

	// The mods <folder>/crashes.toml lists, id -> why they won't load (what Discover leaves out)
	std::map<std::string, std::string> CrashedMods(const std::filesystem::path& folder);

	struct Info
	{
		std::string id, name, version, kind;	// kind: "native", "lua", "native + lua" or "content"
		std::filesystem::path dir;
		Phase phase = Phase::Engine;
		bool loaded = false;
		int loadIndex = -1;						// Order it loaded in (0 first), -1 if it didn't
		int reloads = 0;
		std::string problem;					// Why it isn't loaded (left out, failed, crashed)
		std::vector<std::string> modules;		// Names of the modules it registered
		std::vector<std::string> warnings;
	};
	// Every mod found, loaded or not, in load order then the rest
	std::vector<Info> List();
}
