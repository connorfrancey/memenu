#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Named save states, kept per level in memenu/savestates/<level>.json so they survive restarting the game. A stored
// state is a player_state snapshot: saving one also makes it the Save State, loading one makes it the Save State and
// loads it, so the Load State key keeps going back to it.
//
// The list follows the level the player is in: it is read from that level's file when the level changes. All functions
// are thread safe; the ones that touch the player say "game thread".
namespace save_library
{
	struct Entry
	{
		std::string name;
		std::string created;    // "2026-11-04 14:03"
		float x = 0.f, y = 0.f, z = 0.f;
		float speed = 0.f;      // Horizontal, units per second
	};

	// The states of `map` (empty: the level the menu last saw). Reads its file the first time.
	std::vector<Entry> List(const std::string& map = {});
	std::string Map();          // The level the list is for, lower case; empty outside a level

	int Selected();             // -1: none
	void Select(int index);
	void SelectNext(int step);  // Wraps around

	// Game thread. False with the reason in `error`.
	bool SaveNew(std::string* error = nullptr);         // Saves the player as a new state and selects it
	bool Overwrite(int index, std::string* error = nullptr);
	bool Load(int index, std::string* error = nullptr);

	void Rename(int index, const std::string& name);
	void Remove(int index);
	void MoveBy(int index, int step);   // Up (-1) or down (+1) in the list

	// Where the files go (default: <menu folder>/savestates). The self-test points it elsewhere.
	void SetFolder(const std::filesystem::path& folder);
	std::filesystem::path Folder();
}
