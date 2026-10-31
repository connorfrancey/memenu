#include "game/object_ref.h"

#include <mutex>
#include <unordered_map>

namespace game 
{
	namespace 
	{
		std::mutex g_nameMutex;
		std::unordered_map<std::string, int> g_names;
		cache::Stats& g_nameStats = cache::Register("FName lookups", cache::Scope::Manual);

	}
	
	bool IsDefaultObject(Classes::UObject* object) 
	{
		return object->GetName().rfind("Default__", 0) == 0;
	}

	int NameIndex(const char* name) 
	{
		{
			std::lock_guard lock(g_nameMutex);
			if (auto it = g_names.find(name); it != g_names.end())
			{
				g_nameStats.hits.fetch_add(1, std::memory_order_relaxed);
				return it->second;
			}
		}

		// Walk the engine's name table once (outside the lock: it can take a while).
		g_nameStats.misses.fetch_add(1, std::memory_order_relaxed);
		const clk::Ticks t0 = clk::Now();
		int found = -1;
		auto& names = Classes::FName::GetGlobalNames();
		for (std::size_t i = 0; i < names.Num(); ++i)
		{
			const Classes::FNameEntry* entry = names[i];
			if (entry && entry->GetName() == name)
			{
				found = static_cast<int>(i);
				break;
			}
		}
		g_nameStats.computeTicks.fetch_add(clk::Now() - t0, std::memory_order_relaxed);

		// Only remember hits: a name may be added to the table later (when its package loads).
		if (found >= 0)
		{
			std::lock_guard lock(g_nameMutex);
			g_names.emplace(name, found);
		}
		return found;
	}

	Classes::FName Name(const char* name)
	{
		const int index = NameIndex(name);
		return Classes::FName(index < 0 ? 0 : index);
	}
}