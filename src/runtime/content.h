#pragma once
// Content mods (Mirage, step 7): what a mod changes in the game as data, no code. Its content folders (mod.toml
// `content = ["content"]`) may hold:
//
//   defaults.toml   property overrides: one table per object, its properties by dotted path
//
//       ["TdGame.Default__TdPawn"]          # an object by path: a class's default object here (new pawns get it)
//       MaxStepHeight = 60.0
//       ["TdPawn TdGame.Default__TdPawn"]   # or by full name ("Class Path"), when the path alone is ambiguous
//       ["TdGame.Default__TdPlayerPawn"]
//       instances = true                    # also every object of that class that exists (the player's pawn now)
//
//   Values are TOML booleans, integers, floats and strings (names); a table sets a struct's members.
//
// Applied on the game thread, re-applied after every level load (objects that came with the level), and undone
// exactly (each value's bytes as they were) when the content is turned off or the mod unloads. The game's files are
// never written.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mirage::content
{
	class Set;		// One mod's content

	// Reads the content folders of a mod. Problems (an unreadable file, a bad value) go to `problems`; what could be
	// read is kept.
	std::shared_ptr<Set> Load(const std::string& modId, const std::vector<std::filesystem::path>& folders,
							  std::vector<std::string>& problems);
	bool Empty(const Set& set);

	// On / off. Game thread.
	void Apply(Set& set);
	void Restore(Set& set);
	bool Applied(const Set& set);

	// Every applied set again: after a level load (new instances, objects of the level). Game thread.
	void Register(std::shared_ptr<Set> set);
	void Unregister(const Set& set);
	void ReapplyAll();

	struct Stats
	{
		int objects = 0, values = 0, missing = 0;	// Values applied; missing: objects or properties not found
		std::vector<std::string> notes;				// The first problems
	};
	Stats GetStats(const Set& set);
}
