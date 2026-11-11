#pragma once
// Texture replacement, in the game (Mirage, step 7): mods' replacements turned on and off, and the rewritten packages
// that carry them (runtime/texture_files.h) built, cached and opened in place of the originals (runtime/file_redirect.h).
//
//   1. The index: which packages hold a copy of which texture, read from every package header under CookedPC once and
//      kept in <menu>\cache\textures\index.txt (only packages whose size or time changed are read again).
//   2. Per package that holds a replaced texture, a rewritten copy in <menu>\cache\textures\overlays, named by a hash
//      of the original and of the images. It is made the first time the game opens that package (in the opening
//      thread: a fraction of a second for a map, uncompressed so it is quick) and reused until one of them changes.
//      Only the packages the game loads get a copy: a common texture is in about a hundred.
//   3. The game's open of that package opens the copy: the replacement shows from the next level load on. Turned off,
//      the redirect goes and the next load is the original again.
//
// The index is kept up to date on a background thread; Activate / Deactivate only record what is wanted. Several mods
// replacing the same texture: the one activated last wins.

#include "runtime/texture_files.h"

#include <string>
#include <vector>

namespace mirage::textures
{
	// `owner`'s replacements on (again: the same list is a no-op) / off. Any thread.
	void Activate(const std::string& owner, std::vector<Replacement> replacements);
	void Deactivate(const std::string& owner);

	struct Status
	{
		bool busy = false;					// Indexing or building
		int packages = 0;					// Packages holding a replaced texture (opened as copies)
		int ready = 0;						// ... whose copy existed already when the replacements were turned on
		int textures = 0;					// Replacements found in some package
		std::size_t indexedPackages = 0;	// Packages in the index
		double indexSeconds = 0.0;			// The last index update
		double buildSeconds = 0.0;			// Making copies, so far
		std::vector<std::string> problems;	// Of `owner` (all, if empty): images that can't be used, textures in no package
	};
	Status GetStatus(const std::string& owner = {});

	// Waits until no work is left (for the self-test). False on timeout.
	bool WaitIdle(double seconds);

	// The worker ends and the redirects go (at unload, before MinHook is shut down).
	void Shutdown();
}
