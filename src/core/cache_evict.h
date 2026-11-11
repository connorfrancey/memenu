#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// The eviction manager: one memory budget for every keyed cache in the menu (cache::Lru, cache::TtlMap, cache::Memo,
// the object caches and indexes). Each cache bounds itself by entry count; this bounds them all together, and decides
// who gives memory back by how much each is worth: the caches with the fewest hits per kilobyte since the last pass go
// first, so a cache that is in use keeps its entries while an idle one shrinks.
//
//   class MyCache : cache::Evictable { std::size_t Bytes() const override; std::size_t Trim(std::size_t) override; };
//   cache::Evictions().Add(myCache, "My cache");         // or derive from cache::Evictor::Member (registers itself)
//
// Enforce() runs from the cache scheduler (core/cache_sched.h) on the game thread, like the caches' own code; Add,
// Remove and Snapshot may be called from any thread. Sizes are estimates (entry count x a per-entry size), good enough
// to compare caches and keep the total near the budget.
namespace cache
{
	class Evictable
	{
	public:
		virtual ~Evictable() = default;
		virtual std::size_t Bytes() const = 0;					// About how much it holds now
		// Gives back about `bytes`, least valuable entries first (least recently used, soonest to expire, ...);
		// returns what it freed. May free all it has.
		virtual std::size_t Trim(std::size_t bytes) = 0;
		virtual std::size_t Expire() { return 0; }				// Drops what went stale (TTL caches); entries dropped
		virtual std::uint64_t Hits() const { return 0; }		// Hits so far (its statistics): how much it is worth
	};

	class Evictor
	{
	public:
		explicit Evictor(std::size_t budgetBytes = 16u << 20);

		void Add(Evictable& cache, std::string name);
		void Remove(Evictable& cache);

		void SetBudget(std::size_t bytes);
		std::size_t Budget() const;

		struct Pass
		{
			std::size_t before = 0, after = 0;		// Total bytes
			std::size_t expired = 0;				// Entries dropped as stale
			int trimmed = 0;						// Caches that gave memory back
		};
		// Expires stale entries everywhere, then, over the budget, trims the least valuable caches down to 90% of it
		Pass Enforce();

		struct View
		{
			std::string name;
			std::size_t bytes = 0;
			std::uint64_t hits = 0;
			std::uint64_t trims = 0;			// Times it was asked to give memory back
			std::size_t trimmedBytes = 0;		// In total
		};
		std::vector<View> Snapshot() const;		// As of the last pass
		std::size_t Total() const;				// As of the last pass
		Pass Last() const;

		// A cache that registers itself for its lifetime
		class Member : public Evictable
		{
		public:
			Member(const Member&) = delete;
			Member& operator=(const Member&) = delete;
		protected:
			explicit Member(std::string name);
			~Member() override;
		};

	private:
		struct Entry
		{
			Evictable* cache;
			std::string name;
			std::size_t bytes = 0;
			std::uint64_t hitsAtPass = 0, hits = 0;
			std::uint64_t trims = 0;
			std::size_t trimmedBytes = 0;
		};
		mutable std::mutex lock_;
		std::vector<Entry> entries_;
		std::size_t budget_;
		std::size_t total_ = 0;
		Pass last_;
	};

	Evictor& Evictions();		// The menu's
}
