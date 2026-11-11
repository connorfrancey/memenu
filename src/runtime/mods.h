#pragma once
// Mirage mods (step 4): the runtime's side of api/mirage/api.h, and loading the mods in a folder.
//
// Each mod is a folder with a mod.toml (runtime/manifest.h), and a DLL (entry) and/or Lua (main.lua). Its modules are
// ordinary menu modules (Module subclasses), so they show in the menu, save their settings in the menu's config and
// run through the scheduler like the built-in ones.
//
// Step 4 loads mods in id order, after checking that the runtime version and the required mods (present, in range) and
// incompatible ones (absent) allow it. Load order by dependencies, load phases, hot reload and crash attribution are
// step 5.

#include "mirage/api.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mirage::mods
{
	inline constexpr const char* kRuntimeVersion = "0.1.0";

	// The API table every mod gets (Lua mods too, through runtime/mod_lua.cpp)
	const MirageApi* Api();

	// Loads every mod in `folder` (its subfolders with a mod.toml). Call after modules::Init and before config::Load
	// and sched::Init, from the menu's thread. Adding to the mods already loaded is fine (a second folder).
	void LoadAll(const std::filesystem::path& folder);
	// Unloads them in reverse order: after modules::DisableAll and the hooks are gone (nothing calls a mod any more)
	void UnloadAll();

	struct Info
	{
		std::string id, name, version, kind;	// kind: "native", "lua" or "native + lua"
		std::filesystem::path dir;
		bool loaded = false;
		std::string problem;					// Why it wasn't loaded
		std::vector<std::string> modules;		// Names of the modules it registered
		std::vector<std::string> warnings;
	};
	// Every mod found, loaded or not
	std::vector<Info> List();
}
