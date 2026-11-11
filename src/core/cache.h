#pragma once
#include "clock.h"
#include "optimizations.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Smart caching for values that are expensive to work out (object scans, name lookups, list building).
//
// A cache::Value<T> recomputes its value only when it went stale. When that happens depends on its Scope:
//
//   Tick      once per engine tick (many callers in one tick share one computation)
//   Level     until the next level load
//   Timed     until `ttl` seconds have passed
//   Manual    until Invalidate()
//
// Every cache registers named statistics (hits, misses, time spent recomputing) that the performance monitor shows.
// A Value belongs to one thread (in practice the game thread): it is not locked. The statistics are atomics, so any
// thread may read them. Names of game objects: see game/object_ref.h.
//
// More caches, each in its own header:
//   core/ttl_cache.h         cache::TtlMap       keyed values that expire a set time after they were stored
//   core/lru_cache.h         cache::Lru          keyed values, at most N: the least recently used one goes first
//   core/memory_cache.h      mem::Cache          values read from game memory (crash-safe), per tick/level/TTL
//   core/signature_cache.h   sig::Cache          byte pattern scan results, kept on disk for the same game exe
//   core/address_cache.h     addr::Cache         named addresses and pointer chains
//   core/function_cache.h    cache::Memo         a function's results by its arguments (memoization)
//   core/negative_cache.h    cache::NegativeCache  lookups that failed, not tried again for a while
//   core/string_cache.h      str::Intern         one shared copy of each name; compared by pointer
//   core/hardware.h          hw::Get             CPU, memory, OS and GPU, queried once
//   game/object_cache.h      game::FindObject    game objects by full name, checked alive on every use
//   game/object_query.h      game::ObjectsOf     the query cache: an index of the game's objects by class and name
//                            game::FindByPath    (built in slices), and game::ActorsOf, the levels' actors per tick
//
// Over all of them:
//   core/cache_evict.h       cache::Evictions    one memory budget: the coldest caches give memory back first
//   core/cache_sched.h       cache::Maintenance  the caches' upkeep in a small time slice of each tick, jobs in turn
namespace cache
{
	// Tick/Level/Timed/Manual: when a value goes stale (see above). Lru and Persistent only label statistics: an LRU
	// cache (bounded by size, not time) and one that is kept on disk between sessions.
	enum class Scope : std::uint8_t { Tick, Level, Timed, Manual, Lru, Persistent };
	
	// ---- Epochs (bumped by the game layer) ---------------------------------------------------------------------------------------------------
	std::uint32_t TickEpoch();
	std::uint32_t LevelEpoch();
	void NextTick();				// Game thread, once per engine tick
	void NextLevel();				// A level load started
	
	// ---- Statistics --------------------------------------------------------------------------------------------------------------------------
	struct Stats 
	{
		std::string name;
		Scope scope = Scope::Manual;
		std::atomic<std::uint64_t> hits{0}, misses{0};
		std::atomic<std::uint64_t> computeTicks{0};			// Clock ticks spent computing (all misses)
		std::atomic<std::uint64_t> invalidations{0};
		std::atomic<std::uint64_t> evictions{0};			// Keyed caches: entries dropped to make room or expired
		std::atomic<std::uint64_t> entries{0};				// Keyed caches: entries held now (1 or 0 for a Value)
	};
	// Stats live as long as the process (never freed), so a pointer to them is always valid.
	Stats& Register(std::string name, Scope scope);
	
	struct StatsView 
	{
		std::string name;
		Scope scope;
		std::uint64_t hits, misses, invalidations, evictions, entries;
		double computeSeconds;
	};
	std::vector<StatsView> Snapshot();

	const char* ScopeName(Scope scope);

	// ---- Freshness, shared by every cache ----------------------------------------------------------------------------------------------------
	// The stamp to store with a value computed now, and whether a value stored with `stamp` is still fresh. Timed stamps
	// are clock ticks, Tick/Level ones epochs; Manual/Lru/Persistent values never go stale on their own.
	inline std::uint64_t StampFor(Scope scope, clk::Ticks now)
	{
		switch (scope)
		{
			case Scope::Tick: return TickEpoch();
			case Scope::Level: return LevelEpoch();
			case Scope::Timed: return now;
			default: return 0;
		}
	}
	inline bool IsFresh(Scope scope, std::uint64_t stamp, clk::Ticks ttl, clk::Ticks now)
	{
		switch (scope)
		{
			case Scope::Tick: return stamp == TickEpoch();
			case Scope::Level: return stamp == LevelEpoch();
			case Scope::Timed: return now - stamp < ttl;
			default: return true;
		}
	}

	// Times a computation into `stats` (one miss).
	template <class F>
	decltype(auto) Compute(Stats& stats, F&& compute)
	{
		struct Timer
		{
			Stats& s;
			clk::Ticks t0 = clk::Now();
			~Timer()
			{
				s.misses.fetch_add(1, std::memory_order_relaxed);
				s.computeTicks.fetch_add(clk::Now() - t0, std::memory_order_relaxed);
			}
		} timer{stats};
		return std::forward<F>(compute)();
	}
	
	// ---- A cached value ----------------------------------------------------------------------------------------------------------------------
	template <class T>
	class Value 
	{
	public:
		// `ttlSeconds` is only used by Scope::Timed.
		Value(std::string name, Scope scope, double ttlSeconds = 0.0)
			: stats_(&Register(std::move(name), scope)), scope_(scope), ttl_(clk::FromSeconds(ttlSeconds)) {}

		// The cached value, recomputed with `compute()` (returns a T) when it went stale.
		template <class F>
		const T& Get(F&& compute)
		{
			if (Fresh())
			{
				stats_->hits.fetch_add(1, std::memory_order_relaxed);
				return value_;
			}
			value_ = Compute(*stats_, std::forward<F>(compute));
			stamp_ = StampFor(scope_, clk::Now());
			if (!valid_) stats_->entries.store(1, std::memory_order_relaxed);
			valid_ = true;
			return value_;
		}

		void Invalidate()
		{
			if (valid_)
			{
				stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
				stats_->entries.store(0, std::memory_order_relaxed);
			}
			valid_ = false;
		}

		bool Valid() const { return valid_; }

	private:
		bool Fresh() const
		{
			if (!opt::kEnabled || !valid_) return false;	// No optimizations: recompute on every Get()
			return IsFresh(scope_, stamp_, ttl_, scope_ == Scope::Timed ? clk::Now() : 0);
		}
		
		Stats* stats_;
		Scope scope_;
		clk::Ticks ttl_;
		bool valid_ = false;
		std::uint64_t stamp_ = 0;
		T value_{};
	};
}