#pragma once
#include "core/cache.h"
#include "core/lru_cache.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

// Reading game memory, and caching what was read.
//
// mem::Read never crashes on a bad address: it reads through ReadProcessMemory on this process, which fails instead of
// faulting (an unmapped page, a guard page, a freed object). That costs about a microsecond, so values read many times
// a tick (by several modules, every frame of an overlay) are better read once through a mem::Cache:
//
//   mem::Cache& c = mem::TickCache();                      // shared: everything read this engine tick
//   if (auto speed = c.Get<float>(pawn + 0x1F4)) ...       // read once per tick, then a copy
//
// A Cache keeps the last `capacity` values (least recently used dropped first), each fresh for its scope: Tick (until
// the next engine tick), Level, Timed (`ttl` seconds) or Manual (until invalidated). A failed read is cached the same
// way (nullopt until stale): a bad address isn't retried within its scope. Values are trivially copyable types of at
// most kMaxValue bytes. After writing to an address the menu reads through a cache, Invalidate() it.
//
// Thread-safe (one lock per cache, held only for the lookup). A Tick value read on the render thread may be from the
// previous tick. Without optimizations (MEMENU_OPTIMIZATIONS off) every Get() reads the memory again.
namespace mem
{
	// Crash-safe reads. False / nullopt when any byte of [address, address + size) can't be read.
	bool ReadRaw(std::uintptr_t address, void* out, std::size_t size);
	template <class T>
	std::optional<T> Read(std::uintptr_t address)
	{
		static_assert(std::is_trivially_copyable_v<T>, "mem::Read: T must be trivially copyable");
		T value;
		if (!ReadRaw(address, &value, sizeof value)) return std::nullopt;
		return value;
	}
	inline bool Readable(std::uintptr_t address, std::size_t size)
	{
		unsigned char probe[64];
		while (size)
		{
			const std::size_t n = size < sizeof probe ? size : sizeof probe;
			if (!ReadRaw(address, probe, n)) return false;
			address += n;
			size -= n;
		}
		return true;
	}

	class Cache
	{
	public:
		static constexpr std::size_t kMaxValue = 32;	// Largest cached value (a vector, a rotator, a matrix row...)

		Cache(std::string name, cache::Scope scope = cache::Scope::Tick, double ttlSeconds = 0.0, std::size_t capacity = 4096);

		template <class T>
		std::optional<T> Get(std::uintptr_t address)
		{
			static_assert(std::is_trivially_copyable_v<T>, "mem::Cache: T must be trivially copyable");
			static_assert(sizeof(T) <= kMaxValue, "mem::Cache: T is larger than kMaxValue");
			T value;
			if (!GetRaw(address, &value, sizeof value)) return std::nullopt;
			return value;
		}

		// Raw bytes (size <= kMaxValue). False when they can't be read.
		bool GetRaw(std::uintptr_t address, void* out, std::size_t size);

		// Forget values that overlap [address, address + size): the next Get reads them again.
		void Invalidate(std::uintptr_t address, std::size_t size = 1);
		void Clear();

		std::size_t Size();
		cache::Scope Scope() const { return scope_; }

	private:
		struct Key
		{
			std::uintptr_t address;
			std::uint32_t size;
			bool operator==(const Key& o) const { return address == o.address && size == o.size; }
		};
		struct KeyHash
		{
			std::size_t operator()(const Key& k) const
			{
				return std::hash<std::uintptr_t>{}(k.address) ^ (static_cast<std::size_t>(k.size) * 0x9E3779B9u);
			}
		};
		struct Entry
		{
			std::uint64_t stamp = 0;
			bool ok = false;
			unsigned char bytes[kMaxValue];
		};

		cache::Stats* stats_;
		cache::Scope scope_;
		clk::Ticks ttl_;
		std::mutex mutex_;
		cache::Lru<Key, Entry, KeyHash> values_;
	};

	// Process-wide caches: values read this engine tick, and values that stay until the next level load.
	Cache& TickCache();
	Cache& LevelCache();
}
