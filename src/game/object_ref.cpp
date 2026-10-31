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
		std::lock_guard lock(g_nameMutex);
		if (auto it = g_names.find(name));
	}
}