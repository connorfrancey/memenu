#pragma once
#include "core/cache.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

// cache::TtlMap: keyed values that expire a set time after they were stored (a Timed cache::Value, but one per key).
//
//   cache::TtlMap<std::string, LevelInfo> info{"Level info", 5.0};        // 5 s
//   const LevelInfo& i = info.Get(map, [&] { return ReadLevelInfo(map); });
//
// Expired entries are dropped when they are looked up, and all at once now and then while new ones are stored (at most
// once per TTL). With `maxEntries`, storing into a full map first drops the expired entries, then the one closest to
// expiring. One owner thread, like cache::Value (not locked); the statistics may be read from any thread.
// Without optimizations (MEMENU_OPTIMIZATIONS off) nothing is ever found: Get() computes every time.
namespace cache
{
	template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
	class TtlMap
	{
	public:
		TtlMap(std::string name, double ttlSeconds, std::size_t maxEntries = 0)
			: stats_(&Register(std::move(name), Scope::Timed)), ttl_(clk::FromSeconds(ttlSeconds)), max_(maxEntries) {}

		// The value for `key`, recomputed with `compute()` (returns a V) when missing or expired. The reference is
		// valid until the next call that stores or drops entries.
		template <class F>
		const V& Get(const K& key, F&& compute)
		{
			const clk::Ticks now = clk::Now();
			if (opt::kEnabled)
			{
				auto it = map_.find(key);
				if (it != map_.end())
				{
					if (now < it->second.expires)
					{
						stats_->hits.fetch_add(1, std::memory_order_relaxed);
						return it->second.value;
					}
					it->second.value = Compute(*stats_, std::forward<F>(compute));
					it->second.expires = clk::Now() + ttl_;
					return it->second.value;
				}
			}
			V value = Compute(*stats_, std::forward<F>(compute));
			return Store(key, std::move(value), ttl_, clk::Now());
		}

		// The value for `key`, or nullptr when missing or expired (an expired entry is dropped).
		const V* Find(const K& key)
		{
			if (!opt::kEnabled) { stats_->misses.fetch_add(1, std::memory_order_relaxed); return nullptr; }
			auto it = map_.find(key);
			if (it != map_.end() && clk::Now() >= it->second.expires)
			{
				map_.erase(it);
				Expired(1);
				it = map_.end();
			}
			(it != map_.end() ? stats_->hits : stats_->misses).fetch_add(1, std::memory_order_relaxed);
			return it != map_.end() ? &it->second.value : nullptr;
		}

		// Stores `value` for the map's TTL, or for `ttlSeconds` when given (>= 0).
		void Put(const K& key, V value, double ttlSeconds = -1.0)
		{
			Store(key, std::move(value), ttlSeconds >= 0.0 ? clk::FromSeconds(ttlSeconds) : ttl_, clk::Now());
		}

		bool Erase(const K& key)
		{
			if (!map_.erase(key)) return false;
			stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			UpdateSize();
			return true;
		}

		void Clear()
		{
			if (!map_.empty()) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			map_.clear();
			UpdateSize();
		}

		// Drops every expired entry; returns how many.
		std::size_t Purge()
		{
			const clk::Ticks now = clk::Now();
			std::size_t n = 0;
			for (auto it = map_.begin(); it != map_.end();)
			{
				if (now >= it->second.expires) { it = map_.erase(it); ++n; }
				else ++it;
			}
			nextPurge_ = now + ttl_;
			Expired(n);
			return n;
		}

		std::size_t Size() const { return map_.size(); }		// Expired entries not purged yet count too
		double TtlSeconds() const { return clk::Seconds(ttl_); }

	private:
		struct Entry
		{
			V value;
			clk::Ticks expires;
		};

		const V& Store(const K& key, V value, clk::Ticks ttl, clk::Ticks now)
		{
			auto it = map_.find(key);
			if (it == map_.end())
			{
				if (now >= nextPurge_) Purge();
				if (max_ && map_.size() >= max_) Purge();
				if (max_ && map_.size() >= max_) EvictSoonest();
				it = map_.emplace(key, Entry{std::move(value), now + ttl}).first;
				UpdateSize();
			}
			else
			{
				it->second = Entry{std::move(value), now + ttl};
			}
			return it->second.value;
		}

		void EvictSoonest()
		{
			auto soonest = map_.begin();
			for (auto it = map_.begin(); it != map_.end(); ++it)
				if (it->second.expires < soonest->second.expires) soonest = it;
			if (soonest == map_.end()) return;
			map_.erase(soonest);
			stats_->evictions.fetch_add(1, std::memory_order_relaxed);
		}

		void Expired(std::size_t n)
		{
			if (!n) return;
			stats_->evictions.fetch_add(n, std::memory_order_relaxed);
			UpdateSize();
		}

		void UpdateSize() { stats_->entries.store(map_.size(), std::memory_order_relaxed); }

		Stats* stats_;
		clk::Ticks ttl_;
		std::size_t max_;
		clk::Ticks nextPurge_ = 0;
		std::unordered_map<K, Entry, Hash, Eq> map_;
	};
}
