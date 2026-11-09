#include "core/memory_cache.h"

#include <Windows.h>

#include <vector>

namespace mem
{
	bool ReadRaw(std::uintptr_t address, void* out, std::size_t size)
	{
		if (!size) return true;
		if (!address || address + size < address) return false;	// Null, or wraps around the address space
		SIZE_T read = 0;
		return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &read) && read == size;
	}

	Cache::Cache(std::string name, cache::Scope scope, double ttlSeconds, std::size_t capacity)
		: stats_(&cache::Register(std::move(name), scope)), scope_(scope), ttl_(clk::FromSeconds(ttlSeconds)),
		  values_(*stats_, capacity) {}

	bool Cache::GetRaw(std::uintptr_t address, void* out, std::size_t size)
	{
		if (size > kMaxValue) return ReadRaw(address, out, size);	// Too big to cache: read it
		const Key key{address, static_cast<std::uint32_t>(size)};
		const clk::Ticks now = clk::Now();
		{
			std::lock_guard lock(mutex_);
			if (const Entry* e = values_.Peek(key); opt::kEnabled && e && cache::IsFresh(scope_, e->stamp, ttl_, now))
			{
				values_.Find(key);		// Most recently used now (and counted as a hit)
				if (e->ok) std::memcpy(out, e->bytes, size);
				return e->ok;
			}
		}

		// Read outside the lock: another thread may read the same value meanwhile, which is harmless.
		Entry fresh;
		fresh.ok = cache::Compute(*stats_, [&] { return ReadRaw(address, fresh.bytes, size); });
		fresh.stamp = cache::StampFor(scope_, clk::Now());
		if (fresh.ok) std::memcpy(out, fresh.bytes, size);
		std::lock_guard lock(mutex_);
		values_.Put(key, fresh);
		return fresh.ok;
	}

	void Cache::Invalidate(std::uintptr_t address, std::size_t size)
	{
		const std::uintptr_t end = address + (size ? size : 1);
		std::lock_guard lock(mutex_);
		std::vector<Key> overlapping;
		values_.ForEach([&](const Key& k, const Entry&) {
			if (k.address < end && address < k.address + k.size) overlapping.push_back(k);
		});
		for (const Key& k : overlapping) values_.Erase(k);
	}

	void Cache::Clear()
	{
		std::lock_guard lock(mutex_);
		values_.Clear();
	}

	std::size_t Cache::Size()
	{
		std::lock_guard lock(mutex_);
		return values_.Size();
	}

	Cache& TickCache()
	{
		static Cache c{"Memory (tick)", cache::Scope::Tick};
		return c;
	}

	Cache& LevelCache()
	{
		static Cache c{"Memory (level)", cache::Scope::Level};
		return c;
	}
}
