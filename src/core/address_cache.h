#pragma once
#include "core/cache.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// addr::Cache: game addresses by name, resolved once and reused.
//
// Two kinds:
//   Resolve("ProcessEvent", [] { return ...; })       anything worked out once: a pattern scan plus an offset, a
//                                                     global read out of an instruction. Kept until invalidated.
//   Chain("Player speed", base, {0x1C, 0x2A0, 0x4})  a pointer chain into objects that come and go: fresh for a
//                                                     scope (default: one engine tick), then followed again.
// A chain is followed with crash-safe reads: p = base; for each offset: p = [p] + offset. So {0x1C, 0x4} is
// [[base] + 0x1C] + 0x4: the address of a field, not its value (read that with mem::Read or a mem::Cache).
//
// 0 means "couldn't resolve" and is never cached: the next call tries again. Resolve/Chain can take any scope (Tick,
// Level, Timed with `ttl` seconds, Manual). Thread-safe; a resolver runs outside the lock and may run twice when two
// threads ask at once. Without optimizations (MEMENU_OPTIMIZATIONS off) every call resolves again.
namespace addr
{
	class Cache
	{
	public:
		explicit Cache(std::string name);

		template <class F>
		std::uintptr_t Resolve(std::string_view name, F&& resolve, cache::Scope scope = cache::Scope::Manual,
							   double ttlSeconds = 0.0)
		{
			if (const std::uintptr_t a = Lookup(name)) return a;
			const std::uintptr_t a = cache::Compute(*stats_, std::forward<F>(resolve));
			Store(name, a, scope, ttlSeconds);
			return a;
		}

		std::uintptr_t Chain(std::string_view name, std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets,
							 cache::Scope scope = cache::Scope::Tick, double ttlSeconds = 0.0);

		// The cached address (0 when missing or stale), without resolving or counting.
		std::uintptr_t Peek(std::string_view name);
		// Remembers an address worked out elsewhere (game::Init's globals), so others can Peek it.
		void Set(std::string_view name, std::uintptr_t address, cache::Scope scope = cache::Scope::Manual,
				 double ttlSeconds = 0.0);
		void Invalidate(std::string_view name);
		void InvalidateAll();

		struct View
		{
			std::string name;
			std::uintptr_t address;
			cache::Scope scope;
			std::uint64_t resolves;		// Times it was worked out
		};
		std::vector<View> Snapshot();

	private:
		struct Entry
		{
			std::uintptr_t address = 0;
			cache::Scope scope = cache::Scope::Manual;
			clk::Ticks ttl = 0;
			std::uint64_t stamp = 0;
			std::uint64_t resolves = 0;
		};

		std::uintptr_t Lookup(std::string_view name);		// Counts a hit when fresh
		void Store(std::string_view name, std::uintptr_t address, cache::Scope scope, double ttlSeconds);

		cache::Stats* stats_;
		std::mutex mutex_;
		std::map<std::string, Entry, std::less<>> entries_;
	};

	// Follows a pointer chain now (see Chain above), uncached. 0 when a read fails or a pointer on the way is null.
	std::uintptr_t Follow(std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets);

	// The game's address cache (game::Init fills in the engine globals and hooked functions).
	Cache& Game();
}
