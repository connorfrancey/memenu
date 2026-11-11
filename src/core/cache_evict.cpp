#include "core/cache_evict.h"

#include <algorithm>

namespace cache
{
	Evictor::Evictor(std::size_t budgetBytes) : budget_(budgetBytes) {}

	void Evictor::Add(Evictable& cache, std::string name)
	{
		std::lock_guard lock(lock_);
		Entry e;
		e.cache = &cache;
		e.name = std::move(name);
		entries_.push_back(std::move(e));
	}

	void Evictor::Remove(Evictable& cache)
	{
		std::lock_guard lock(lock_);
		entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.cache == &cache; }),
					   entries_.end());
	}

	void Evictor::SetBudget(std::size_t bytes)
	{
		std::lock_guard lock(lock_);
		budget_ = bytes;
	}

	std::size_t Evictor::Budget() const
	{
		std::lock_guard lock(lock_);
		return budget_;
	}

	Evictor::Pass Evictor::Enforce()
	{
		std::lock_guard lock(lock_);
		Pass pass;
		for (Entry& e : entries_) pass.expired += e.cache->Expire();
		for (Entry& e : entries_) {
			e.bytes = e.cache->Bytes();
			e.hits = e.cache->Hits();
			pass.before += e.bytes;
		}
		std::size_t total = pass.before;
		if (total > budget_) {
			const std::size_t target = budget_ / 10 * 9;		// Some room, so the next pass isn't right away
			// Least valuable first: hits since the last pass per kilobyte held
			std::vector<Entry*> order;
			for (Entry& e : entries_)
				if (e.bytes > 0) order.push_back(&e);
			const auto worth = [](const Entry* e) {
				const double recent = static_cast<double>(e->hits - std::min(e->hits, e->hitsAtPass));
				return recent / (static_cast<double>(e->bytes) / 1024.0 + 1.0);
			};
			std::stable_sort(order.begin(), order.end(), [&](const Entry* a, const Entry* b) { return worth(a) < worth(b); });
			for (Entry* e : order) {
				if (total <= target) break;
				const std::size_t freed = std::min(e->cache->Trim(total - target), e->bytes);
				if (freed == 0) continue;
				total -= freed;
				e->bytes -= freed;
				++e->trims;
				e->trimmedBytes += freed;
				++pass.trimmed;
			}
		}
		for (Entry& e : entries_) e.hitsAtPass = e.hits;
		pass.after = total;
		total_ = total;
		last_ = pass;
		return pass;
	}

	std::vector<Evictor::View> Evictor::Snapshot() const
	{
		std::lock_guard lock(lock_);
		std::vector<View> out;
		for (const Entry& e : entries_) out.push_back({e.name, e.bytes, e.hits, e.trims, e.trimmedBytes});
		return out;
	}

	std::size_t Evictor::Total() const
	{
		std::lock_guard lock(lock_);
		return total_;
	}

	Evictor::Pass Evictor::Last() const
	{
		std::lock_guard lock(lock_);
		return last_;
	}

	Evictor::Member::Member(std::string name) { Evictions().Add(*this, std::move(name)); }
	Evictor::Member::~Member() { Evictions().Remove(*this); }

	Evictor& Evictions()
	{
		static Evictor* e = new Evictor;	// Never destroyed: static caches unregister in their destructors at exit
		return *e;
	}
}
