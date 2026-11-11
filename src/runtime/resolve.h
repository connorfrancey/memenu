#pragma once
// Which mods load, and in what order (Mirage, step 5). Pure: no files, no game, so the unit tests can try every case.
//
//   - A mod loads after the mods it requires and the optional ones that are present, and within that by phase (early,
//     engine, world) and then by id, so the order is the same on every machine.
//   - A mod is left out, with the reason, when: the runtime isn't in its `mirage` range; it was turned off (or
//     crashed the last time: the caller passes those); a required mod is missing, out of range, left out itself, or
//     loads in a later phase; an incompatible mod in range is present; or it is part of a cycle of required mods.
//     Leaving a mod out leaves out the mods that require it.
//   - An optional mod that is missing, out of range or left out only drops the ordering; a cycle through optional
//     dependencies is broken by dropping those edges.

#include "runtime/manifest.h"
#include "runtime/semver.h"

#include <map>
#include <string>
#include <vector>

namespace mirage::mods
{
	struct Resolution
	{
		std::vector<std::string> order;					// Ids to load, in load order
		std::map<std::string, std::string> excluded;	// Id -> why it isn't loaded
		std::vector<std::string> warnings;				// Optional dependencies that couldn't be honoured, ...
	};

	// `off`: mods not to load, id -> reason (turned off by the user, crashed last time)
	Resolution Resolve(const std::vector<Manifest>& mods, const semver::Version& runtime,
					   const std::map<std::string, std::string>& off = {});
}
