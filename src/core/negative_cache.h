#pragma once
#include "core/cache.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

// cache::NegativeCache: remembers lookups that failed, for a while, so a lookup that can't succeed yet (an object that
// isn't loaded, a pattern that isn't there, a file that doesn't exist) isn't paid for again on every call.
//
//   cache::Retry retry;  retry.ticks = 120;                // (C++17: no designated initializers)
//   cache::NegativeCache<std::string> missing{"Missing objects", retry};
//   if (missing.Failed(name)) return nullptr;              // tried recently: don't look again yet
//   T* o = ExpensiveLookup(name);
//   if (!o) missing.Remember(name); else missing.Forget(name);
//
// or in one call: missing.Lookup(name, [&] { return ExpensiveLookup(name); }) (anything that tests false = failed).
//
// When a failure is retried depends on the Retry policy: after `ticks` engine ticks (deterministic: use this in
// anything a TAS run depends on), after `seconds`, or both (whichever comes first); and at once after a level load
// when `untilLevel` is set. With `backoff`, each failure in a row doubles the wait, up to `maxBackoff` times.
// Thread-safe. Without optimizations (MEMENU_OPTIMIZATIONS off) nothing is remembered: every lookup runs.
namespace cache
{
	struct Retry
	{
		std::uint32_t ticks = 0;		// Retry after this many engine ticks (0: not tick based)
		double seconds = 0.0;			// Retry after this long (0: not time based)
		bool untilLevel = true;			// A level load clears every failure
		bool backoff = false;			// Double the wait for each failure in a row
		std::uint32_t maxBackoff = 8;	// ...up to this many times the base wait
	};

	template <class K, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
	class NegativeCache
	{
	public:
		NegativeCache(std::string name, Retry retry)
			: stats_(&Register(std::move(name), Scope::Timed)), retry_(retry), seconds_(clk::FromSeconds(retry.seconds)) {}

		// True while `key` failed recently (counted as a hit: a lookup saved).
		bool Failed(const K& key)
		{
			if (!opt::kEnabled) return false;
			std::lock_guard lock(mutex_);
			auto it = failures_.find(key);
			if (it == failures_.end()) return false;
			if (Expired(it->second))
			{
				it->second.expired = true;		// Kept for the back-off count until Forget() or the next failure
				return false;
			}
			stats_->hits.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// The lookup failed: don't try it again for a while.
		void Remember(const K& key)
		{
			if (!opt::kEnabled) return;
			std::lock_guard lock(mutex_);
			Entry& e = failures_[key];
			e.streak = e.expired || e.streak == 0 ? e.streak + 1 : e.streak;
			e.expired = false;
			e.tick = TickEpoch();
			e.level = LevelEpoch();
			e.time = clk::Now();
			stats_->misses.fetch_add(1, std::memory_order_relaxed);
			stats_->entries.store(failures_.size(), std::memory_order_relaxed);
		}

		// The lookup worked (or the key is gone): no failure to remember any more.
		void Forget(const K& key)
		{
			std::lock_guard lock(mutex_);
			if (failures_.erase(key)) stats_->entries.store(failures_.size(), std::memory_order_relaxed);
		}

		void Clear()
		{
			std::lock_guard lock(mutex_);
			if (!failures_.empty()) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			failures_.clear();
			stats_->entries.store(0, std::memory_order_relaxed);
		}

		// Runs `lookup()` unless `key` failed recently (then returns a default R). A result that tests false is a
		// failure and is remembered; a true one clears the key.
		template <class F>
		auto Lookup(const K& key, F&& lookup) -> decltype(lookup())
		{
			using R = decltype(lookup());
			if (Failed(key)) return R{};
			const clk::Ticks t0 = clk::Now();
			R result = std::forward<F>(lookup)();
			stats_->computeTicks.fetch_add(clk::Now() - t0, std::memory_order_relaxed);
			if (result) Forget(key);
			else Remember(key);
			return result;
		}

		std::size_t Size()
		{
			std::lock_guard lock(mutex_);
			return failures_.size();
		}

		// Drops failures that can be retried by now (they are otherwise kept for their back-off streak).
		std::size_t Purge()
		{
			std::lock_guard lock(mutex_);
			std::size_t n = 0;
			for (auto it = failures_.begin(); it != failures_.end();)
			{
				if (it->second.expired || Expired(it->second)) { it = failures_.erase(it); ++n; }
				else ++it;
			}
			stats_->evictions.fetch_add(n, std::memory_order_relaxed);
			stats_->entries.store(failures_.size(), std::memory_order_relaxed);
			return n;
		}

	private:
		struct Entry
		{
			std::uint32_t tick = 0, level = 0, streak = 0;
			clk::Ticks time = 0;
			bool expired = false;
		};

		// Under mutex_.
		bool Expired(const Entry& e) const
		{
			if (e.expired) return true;
			if (retry_.untilLevel && e.level != LevelEpoch()) return true;
			const std::uint64_t factor = retry_.backoff
				? std::min<std::uint64_t>(std::uint64_t{1} << std::min<std::uint32_t>(e.streak - 1, 30), retry_.maxBackoff)
				: 1;
			if (retry_.ticks && TickEpoch() - e.tick >= retry_.ticks * factor) return true;
			if (seconds_ && clk::Now() - e.time >= seconds_ * factor) return true;
			return !retry_.ticks && !seconds_ && !retry_.untilLevel;	// No policy at all: never remembered
		}

		Stats* stats_;
		Retry retry_;
		clk::Ticks seconds_;
		std::mutex mutex_;
		std::unordered_map<K, Entry, Hash, Eq> failures_;
	};
}
