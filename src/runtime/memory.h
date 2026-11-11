#pragma once
#include <cstdint>

// What the menu's own code has allocated with new (this DLL replaces operator new / delete with counting versions on mimalloc; the
// game's allocations are never counted). Precise where the process's memory isn't: the game allocates and frees all
// the time, so a leak in the menu doesn't show in the process's numbers.
namespace mirage::memory
{
	std::int64_t LiveBytes();		// Allocated with new and not deleted yet, now (any thread)
	std::int64_t TotalAllocations();	// Calls to new so far
}
