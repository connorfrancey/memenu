#include "core/cache.h"

#include <deque>
#include <mutex>

namespace cache 
{
	namespace 
	{
		std::atomic<std::uint32_t> g_tick{1};
		std::atomic<std::uint32_t> g_level{1};
		
		std::mutex g_mutex;
		// A deque never moves its elements: references handed out by Registrar stay valid forever.
		std::deque<Stats>& Registry() 
		{
			static std::deque<Stats> registry;
			return registry;
		}
	}
	
	std::uint32_t TickEpoch() { return g_tick.load(std::memory_order_relaxed); }
	std::uint32_t LevelEpoch() { return g_level.load(std::memory_order_relaxed); }
	void NextTick() { g_tick.fetch_add(1, std::memory_order_relaxed); }
	void NextLevel() { g_level.fetch_add(1, std::memory_order_relaxed); }
	
	Stats& Register(std::string name, Scope scope) 
	{
		std::lock_guard lock(g_mutex);
		Stats& s = Registry().emplace_back();
		s.name = std::move(name);
		s.scope = scope;
		return s;
	}
	
	std::vector<StatsView> Snapshot() 
	{
		std::lock_guard lock(g_mutex);
		std::vector<StatsView> out;
		out.reserve(Registry().size());
		for (const Stats& s : Registry()) 
		{
			out.push_back({s.name, s.scope, s.hits.load(std::memory_order_relaxed), s.misses.load(std::memory_order_relaxed),
							s.invalidations.load(std::memory_order_relaxed),
							clk::Seconds(s.computeTicks.load(std::memory_order_relaxed))});
		}
		return out;
	}
	
	const char* ScopeName(Scope scope) 
	{
		switch (scope) 
		{
			case Scope::Tick: return "tick";
			case Scope::Level: return "level";
			case Scope::Timed: return "timed";
			case Scope::Manual: return "manual";
		}
		return "?";
	}
}