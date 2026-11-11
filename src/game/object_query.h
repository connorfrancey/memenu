#pragma once
// The query cache and the actor cache: the game's objects found without walking the whole object list each time.
//
//   for (auto* fn : game::ObjectsOf<Classes::UFunction>()) ...                // every function (by class)
//   auto* cdo = game::FindByPath("TdGame.Default__TdPawn");                  // by path, no full name needed
//   for (auto* pawn : game::ActorsOf(Classes::APawn::StaticClass())) ...     // the pawns in the levels in play
//
// The object index: GObjects by class and by name (the FName's index), rebuilt in slices by the cache scheduler
// (core/cache_sched.h) and swapped in whole when a pass is done; a level load starts it over. A query reads the index,
// plus the slots added since it was built, and checks every object on the way out (its slot still holds it, with the
// same class). The engine reuses freed slots, so an object made in the last moments may be missed until the next pass:
// ask Freshness::Exact when it must be there (a synchronous pass, a fraction of a millisecond in release).
//
// FindByPath looks the last part of the path up as a name (an integer, no strings built) and compares the path of
// those few objects only, where a scan builds every object's name.
//
// The actor cache: the levels' own actor lists (the persistent level and the visible streaming levels: thousands of
// actors, not the whole object list), by class, once per engine tick. The lists are read where UE3 keeps them
// (ULevel::Actors, right after UObject), trusted only while the persistent level's first actor is the WorldInfo, as
// UE3 makes it; otherwise the actors come from the object index (right, just slower).
//
// Game thread only. Under the eviction manager (core/cache_evict.h).

#include "sdk/SDK.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace game
{
	enum class Freshness : std::uint8_t
	{
		Indexed,	// From the index, plus the newest slots: may miss an object made in a reused slot moments ago
		Exact,		// The index rebuilt now first
	};

	// Live objects of `cls` or a subclass (class default objects too)
	std::vector<Classes::UObject*> ObjectsOf(Classes::UClass* cls, Freshness freshness = Freshness::Indexed);
	template <class T>
	std::vector<T*> ObjectsOf(Freshness freshness = Freshness::Indexed)
	{
		std::vector<T*> out;
		for (Classes::UObject* o : ObjectsOf(T::StaticClass(), freshness)) out.push_back(static_cast<T*>(o));
		return out;
	}

	// The object at `path` ("TdGame.Default__TdPawn", "Engine.Actor.SetLocation"), any class; the first if several
	Classes::UObject* FindByPath(std::string_view path, Freshness freshness = Freshness::Indexed);

	// Actors of `cls` or a subclass in the levels in play, as of this tick
	std::vector<Classes::AActor*> ActorsOf(Classes::UClass* cls);

	struct QueryStats
	{
		std::size_t slots = 0, classes = 0, names = 0;	// In the index
		std::uint64_t passes = 0;						// Index passes completed (sliced or exact)
		std::uint64_t exactPasses = 0;					// ... of them made at once for a query (after a level load)
		std::uint64_t restarts = 0;						// Sliced passes started over (a level load under way)
		double lastPassMs = 0.0;						// Its scanning time, in total
		bool levelListsOk = false;						// The actor cache reads the levels' actor lists
		std::size_t levels = 0, actors = 0;				// In play, at the last actor query
	};
	QueryStats GetQueryStats();

	// The maintenance job and the tick task (game::Init)
	void InitObjectQueries();
}
