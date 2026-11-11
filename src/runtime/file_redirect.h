#pragma once
// The engine's package files, redirected (Mirage, step 7): when the game opens a package that a mod's content changes,
// it opens the rewritten copy instead (kernelbase CreateFileW, and GetFileAttributesExW so its size matches). The
// game folder is never written; taking the redirect away gives the original on the next open (the next level load).
//
// A copy can also be made on demand: the first time the game opens such a package, the builder runs right there, in
// the opening thread (a fraction of a second for a map), and its result is opened and remembered.
//
// Once the game has opened a package, it gets the same file for the rest of the session, whatever the table says
// later: the engine keeps file handles open by name and reads a package through more than one of them, so switching
// files under it mixes two layouts (it hung the loader when tried). A change applies to the packages not opened yet,
// and to all of them after a restart.

#include <filesystem>
#include <functional>
#include <map>
#include <set>

namespace mirage::files
{
	// Hooks in place (idempotent). False if MinHook refused, logged.
	bool Install();
	// Hooks gone (at unload: before MinHook is shut down).
	void Uninstall();

	// Builds the copy of `original` and returns its file (empty: none, the original is opened).
	using Builder = std::function<std::filesystem::path(const std::filesystem::path& original)>;

	// The whole table at once: original -> copy for the copies that exist, and the originals whose copy `build` makes
	// when the game first opens them. Paths are compared as full, case-insensitive paths.
	void Set(const std::map<std::filesystem::path, std::filesystem::path>& copies,
			 const std::set<std::filesystem::path>& onDemand, Builder build);

	// The menu's own reads of the originals, on this thread, while one of these lives: never redirected.
	struct Bypass
	{
		Bypass();
		~Bypass();
		Bypass(const Bypass&) = delete;
		Bypass& operator=(const Bypass&) = delete;
	};

	// Whether an open of `original` now gives a copy (one held for the session, in the table, or to be made on demand)
	bool Redirects(const std::filesystem::path& original);
	// Packages opened this session whose file no longer matches the table (a change waiting for a restart)
	int Held();

	unsigned long long Redirected();	// Opens answered with a copy so far
	unsigned long long Built();			// Copies made on demand so far
}
