#pragma once
#include "clock.h"

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
namespace cache 
{
	enum class Scope : std::uint8_t { Tick, Level, Timed, Manual };
	
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
	};
	// Stats live as long as the process (never freed), so a pointer to them is always valid.
	Stats& Register(std::string name, Scope scope);
	
	struct StatsView 
	{
		std::string name;
		Scope scope;
		std::uint64_t hits, misses, invalidations;
		double computeSeconds;
	};
	std::vector<StatsView> Snapshot();
	
	const char* ScopeName(Scope scope);
	
	// ---- A cached value ----------------------------------------------------------------------------------------------------------------------
	template <class T>
	class Value 
	{
	public:
	private:
		std::uint64_t Stamp(clk::Ticks now) const 
		{
			switch (scope_) 
			{
				case Scope::Tick: return TickEpoch();
				case Scope::Level: return LevelEpoch();
				case Scope::Timed: return now;
				default: return 0;
			}
		}
		
		bool Fresh() const 
		{
			if (!valid_) return false;
			switch (scope_) 
			{
				case Scope::Tick: return stamp_ == TickEpoch();
				case Scope::Level: return stamp_ == LevelEpoch();
				case Scope::Timed: return clk::Now() - stamp_ < ttl_;
				default: return true;
			}
		}
		
		Stats* stats_;
		Scope scope_;
		clk::Ticks ttl_;
		bool valid_ = false;
		std::uint64_t stamp = 0;
		T value_{};
	};
}