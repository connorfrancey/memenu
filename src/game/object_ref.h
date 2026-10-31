#pragma once
#include "core/cache.h"

#include <mirrorsedge_sdk.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Cached references to game objects and names. Game thread only.
namespace game 
{
	// A UObject found by scanning GObjects, kept between calls without ever touching freed memory: the pointer is only
	// trusted while its GObjects slot still holds that very pointer (the engine clears the slot when it destroys the
	// object, and the check reads the slot, not the object).
	//
	// When a scan finds nothing, the next scan waits `retryTicks` engine ticks (and a level load lets it scan at once):
	// screens without the object (menus, loading) don't pay for a scan of ~100k objects on every call. Counting ticks,
	// not seconds, keeps it deterministic in a TAS run.
	template <class T>
	class ObjectRef 
	{
	public:
	private:
		cache::Stats* stats_;
		std::uint32_t retry_;
		T* ptr = nullptr;
		std::size_t index_ = 0;
		bool missed_ = false;
		std::uint32_t missTick_ = 0, missLevel_ = 0;
	};
	
	// True for class default objects ("Default__X"), which most scans want to skip.
	bool IsDefaultObject(Classes::UObject* object);
	
	// FName index of `name`, looked up in the engine's name table once and remembered (the SDK's FName(const char*) walks every name on each call). -1 if the engine doesn't have it. Any thread.
	int NameIndex(const char* name);
	Classes::FName Name(const char* name);
}