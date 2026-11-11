#pragma once
// The menu's first-party mods (Mirage, step 9): the menu's own features written as mods against the public API
// (the repository's mods/ folder). They ship inside the DLL (cmake/EmbedMods.cmake) and are written to the mods folder at
// startup, so that every way the menu gets installed has them, at the DLL's version: a file that differs from the
// embedded one is written again, the mod's config.toml (and anything else of the player's) is left alone. They then
// load like any mod: switched off in the menu, never deleted (written again next start).

#include <cstddef>
#include <filesystem>
#include <span>

namespace mirage::firstparty
{
	struct File
	{
		const char* path;					// "<mod id>/main.lua", '/' separated
		const unsigned char* data;
		std::size_t size;
	};
	std::span<const File> Files();			// Generated

	// Writes them under `modsDir` where missing or different. Returns how many files were written.
	int Install(const std::filesystem::path& modsDir);
}
