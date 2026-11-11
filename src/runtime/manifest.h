#pragma once
// mod.toml: what a mod is, read with toml11 (Mirage, step 4).
//
//     id = "hello"                       # Required: a-z, 0-9, '-', '_'; 2 to 64 characters; the folder's name
//     name = "Hello"                     # Shown in the menu (default: the id)
//     version = "1.0.0"                  # Required: semver
//     description = "Says hello"
//     authors = ["Someone"]
//     mirage = ">=0.1"                   # Runtime versions it works with (default: any)
//     game = "1.0"                       # Game build it was made for (informative for now)
//     entry = "hello.dll"                # Native code: a DLL in the mod's folder
//     lua = "main.lua"                   # Lua code (default: main.lua when it exists)
//     phase = "engine"                   # When it loads: early, engine (default) or world
//     content = ["content"]              # Folders with packages, textures, script patches (step 7)
//
//     [dependencies]                     # Required, with version ranges
//     other-mod = "^1.2"
//     [optional]                         # Loaded first when present
//     [incompatible]                     # Not loaded together
//
//     [settings.greeting]                # Settings, declared like Geode's mod.json: one table per key
//     type = "string"                    # bool, int, float, string, choice or key
//     name = "Greeting"                  # Shown in the settings panel (default: the key)
//     description = "What it draws"      # Tooltip
//     default = "Hello"                  # Required, of the type's kind (choice: one of the options; key: a key code)
//     min = 0                            # int and float: the range (both or neither)
//     max = 10
//     format = "%.2f"                    # float: how the value is shown
//     options = ["a", "b"]               # choice: the items
//     module = "Hello"                   # The module whose panel shows it (default: the mod's first module)
//
// The runtime makes them when their module registers, the menu's config saves their values, and code finds them
// by key (MirageApi::setting_find, mirage.setting("greeting") in Lua).
//
// Unknown keys are reported as warnings (a typo in a key would otherwise be silently ignored).

#include "runtime/semver.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mirage::mods
{
	enum class Phase : std::uint8_t { Early, Engine, World };
	const char* PhaseName(Phase p);

	struct SettingDecl
	{
		enum class Type : std::uint8_t { Bool, Int, Float, String, Choice, Key };
		std::string key, name, description, module, format;
		Type type = Type::Bool;
		bool boolDefault = false;
		std::int64_t intDefault = 0;		// Int, Key; Choice: the index of the default option
		double floatDefault = 0.0;
		std::string stringDefault;
		bool hasRange = false;
		double min = 0.0, max = 0.0;
		std::vector<std::string> options;
	};
	const char* SettingTypeName(SettingDecl::Type t);

	struct Dependency
	{
		std::string id;
		semver::Range range;
	};

	struct Manifest
	{
		std::string id, name, description, game;
		semver::Version version;
		std::vector<std::string> authors;
		semver::Range mirage = semver::Range::Any();
		std::string entry;				// "" = no native code
		std::string lua;				// "" = no Lua code (resolved against the folder by Load)
		Phase phase = Phase::Engine;
		std::vector<std::string> content;
		std::vector<Dependency> required, optional, incompatible;
		std::vector<SettingDecl> settings;	// In the order of the file
		std::filesystem::path dir;		// Load: the folder mod.toml is in
	};

	struct Parsed
	{
		std::optional<Manifest> manifest;	// nullopt when there are errors
		std::vector<std::string> errors, warnings;
	};

	// `text` is the content of a mod.toml, `source` names it in messages. Doesn't touch the file system: a missing
	// `lua` stays empty (Load fills in main.lua when the file exists).
	Parsed Parse(std::string_view text, const std::string& source = "mod.toml");
	// <dir>/mod.toml: Parse, plus the checks that need the folder (the files it names exist, the id is the folder's
	// name)
	Parsed Load(const std::filesystem::path& dir);
}
