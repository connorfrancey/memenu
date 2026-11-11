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
