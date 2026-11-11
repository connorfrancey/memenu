#pragma once
#include "core/cache.h"
#include "core/cache_evict.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

// cache::Lru: at most `capacity` keyed values; storing one more drops the least recently used. Values don't go stale
// on their own: use it for results that stay right but are too many to keep all of (names of objects by index, built
// meshes, formatted labels, ...). For values that do go stale, see cache::TtlMap or a cache::Value.
//
//   cache::Lru<std::uint32_t, std::string> names{"Object names", 4096};
//   const std::string& n = names.Get(index, [&] { return FullName(index); });
//
// Get/Find/Put are O(1). One owner thread, like cache::Value (not locked); the statistics may be read from any thread.
// Without optimizations (MEMENU_OPTIMIZATIONS off) nothing is ever found: Get() computes every time.
namespace cache
{
	// Under the eviction manager (core/cache_evict.h): it gives back its least recently used entries first.
	template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
	class Lru : public Evictor::Member
	{
	public:
		Lru(std::string name, std::size_t capacity) : Lru(Register(std::move(name), Scope::Lru), capacity) {}
		// Counted into existing statistics (a cache built on top of this one).
		Lru(Stats& stats, std::size_t capacity)
			: Evictor::Member(stats.name), stats_(&stats), capacity_(std::max<std::size_t>(capacity, 1)) {}

		// ---- Evictable: an estimate per entry (the list and hash nodes, the key and value themselves)
		static constexpr std::size_t kEntryBytes = sizeof(std::pair<K, V>) + sizeof(K) + 6 * sizeof(void*);
		std::size_t Bytes() const override { return order_.size() * kEntryBytes; }
		std::size_t Trim(std::size_t bytes) override
		{
			std::size_t freed = 0;
			while (freed < bytes && !order_.empty()) {
				EvictOne();
				freed += kEntryBytes;
			}
			return freed;
		}
		std::uint64_t Hits() const override { return stats_->hits.load(std::memory_order_relaxed); }

		// The value for `key`, computed with `compute()` (returns a V) when missing. The reference is valid until the
		// next call that stores or drops entries.
		template <class F>
		V& Get(const K& key, F&& compute)
		{
			if (V* v = Lookup(key))
			{
				stats_->hits.fetch_add(1, std::memory_order_relaxed);
				return *v;
			}
			V value = Compute(*stats_, std::forward<F>(compute));
			return Put(key, std::move(value));
		}

		// The value for `key` (now the most recently used), or nullptr.
		V* Find(const K& key)
		{
			V* v = Lookup(key);
			(v ? stats_->hits : stats_->misses).fetch_add(1, std::memory_order_relaxed);
			return v;
		}

		// The value without touching its place in the order or the statistics, or nullptr.
		const V* Peek(const K& key) const
		{
			const auto it = index_.find(key);
			return it != index_.end() ? &it->second->second : nullptr;
		}

		// Stores (or replaces) the value for `key`, as the most recently used.
		V& Put(const K& key, V value)
		{
			auto it = index_.find(key);
			if (it != index_.end())
			{
				it->second->second = std::move(value);
				order_.splice(order_.begin(), order_, it->second);
				return it->second->second;
			}
			while (order_.size() >= capacity_) EvictOne();
			order_.emplace_front(key, std::move(value));
			index_.emplace(key, order_.begin());
			UpdateSize();
			return order_.front().second;
		}

		bool Erase(const K& key)
		{
			auto it = index_.find(key);
			if (it == index_.end()) return false;
			order_.erase(it->second);
			index_.erase(it);
			stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			UpdateSize();
			return true;
		}

		void Clear()
		{
			if (!order_.empty()) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			order_.clear();
			index_.clear();
			UpdateSize();
		}

		// Shrinking drops the least recently used entries at once.
		void SetCapacity(std::size_t capacity)
		{
			capacity_ = std::max<std::size_t>(capacity, 1);
			while (order_.size() > capacity_) EvictOne();
		}

		std::size_t Size() const { return order_.size(); }
		std::size_t Capacity() const { return capacity_; }

		// Keys from most to least recently used (tests, debugging).
		template <class Fn>
		void ForEach(Fn&& fn) const
		{
			for (const auto& [k, v] : order_) fn(k, v);
		}

	private:
		using List = std::list<std::pair<K, V>>;

		V* Lookup(const K& key)
		{
			if (!opt::kEnabled) return nullptr;		// No optimizations: never found
			auto it = index_.find(key);
			if (it == index_.end()) return nullptr;
			order_.splice(order_.begin(), order_, it->second);
			return &it->second->second;
		}

		void EvictOne()
		{
			if (order_.empty()) return;
			index_.erase(order_.back().first);
			order_.pop_back();
			stats_->evictions.fetch_add(1, std::memory_order_relaxed);
			UpdateSize();
		}

		void UpdateSize() { stats_->entries.store(order_.size(), std::memory_order_relaxed); }

		Stats* stats_;
		std::size_t capacity_;
		List order_;			// Front: most recently used
		std::unordered_map<K, typename List::iterator, Hash, Eq> index_;
	};
}
