#pragma once
#include "core/cache.h"

#include <ME_SDK.hpp>

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
		explicit ObjectRef(std::string name, std::uint32_t retryTicks = 120)
			: stats_(&cache::Register(std::move(name), cache::Scope::Level)), retry_(retryTicks) {}

		// The remembered object if it is still alive, else the first object `match(UObject*)` accepts (or nullptr).
		template <class Match>
		T* Find(Match&& match)
		{
			auto& objects = Classes::UObject::GetGlobalObjects();
			if (opt::kEnabled && ptr_ && index_ < objects.Num() && objects.GetByIndex(index_) == ptr_)
			{
				stats_->hits.fetch_add(1, std::memory_order_relaxed);
				return ptr_;
			}
			ptr_ = nullptr;

			// Nothing last time: wait before walking the object list again, unless a level was loaded since.
			// (No optimizations: scan every time.)
			if (opt::kEnabled && missed_ && cache::LevelEpoch() == missLevel_ && cache::TickEpoch() - missTick_ < retry_)
				return nullptr;

			stats_->misses.fetch_add(1, std::memory_order_relaxed);
			const clk::Ticks t0 = clk::Now();
			for (std::size_t i = 0; i < objects.Num(); ++i)
			{
				Classes::UObject* o = objects.GetByIndex(i);
				if (o && match(o))
				{
					ptr_ = static_cast<T*>(o);
					index_ = i;
					break;
				}
			}
			stats_->computeTicks.fetch_add(clk::Now() - t0, std::memory_order_relaxed);
			missed_ = ptr_ == nullptr;
			missTick_ = cache::TickEpoch();
			missLevel_ = cache::LevelEpoch();
			return ptr_;
		}

		void Reset()
		{
			if (ptr_) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			ptr_ = nullptr;
			missed_ = false;
		}

	private:
		cache::Stats* stats_;
		std::uint32_t retry_;
		T* ptr_ = nullptr;
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