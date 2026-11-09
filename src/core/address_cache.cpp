#include "core/address_cache.h"
#include "core/memory_cache.h"

namespace addr
{
	std::uintptr_t Follow(std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets)
	{
		std::uintptr_t p = base;
		for (const std::ptrdiff_t off : offsets)
		{
			const auto next = mem::Read<std::uintptr_t>(p);
			if (!next || !*next) return 0;
			p = *next + static_cast<std::uintptr_t>(off);
		}
		return p;
	}

	Cache::Cache(std::string name) : stats_(&cache::Register(std::move(name), cache::Scope::Manual)) {}

	std::uintptr_t Cache::Chain(std::string_view name, std::uintptr_t base, std::initializer_list<std::ptrdiff_t> offsets,
								cache::Scope scope, double ttlSeconds)
	{
		return Resolve(name, [&] { return Follow(base, offsets); }, scope, ttlSeconds);
	}

	std::uintptr_t Cache::Lookup(std::string_view name)
	{
		if (!opt::kEnabled) return 0;
		std::lock_guard lock(mutex_);
		const auto it = entries_.find(name);
		if (it == entries_.end() || !it->second.address) return 0;
		const Entry& e = it->second;
		if (!cache::IsFresh(e.scope, e.stamp, e.ttl, e.scope == cache::Scope::Timed ? clk::Now() : 0)) return 0;
		stats_->hits.fetch_add(1, std::memory_order_relaxed);
		return e.address;
	}

	void Cache::Store(std::string_view name, std::uintptr_t address, cache::Scope scope, double ttlSeconds)
	{
		std::lock_guard lock(mutex_);
		auto it = entries_.find(name);
		if (it == entries_.end()) it = entries_.emplace(std::string(name), Entry{}).first;
		Entry& e = it->second;
		e.address = address;			// 0 is kept as "not resolved": Lookup never returns it
		e.scope = scope;
		e.ttl = clk::FromSeconds(ttlSeconds);
		e.stamp = cache::StampFor(scope, clk::Now());
		++e.resolves;
		stats_->entries.store(entries_.size(), std::memory_order_relaxed);
	}

	std::uintptr_t Cache::Peek(std::string_view name)
	{
		std::lock_guard lock(mutex_);
		const auto it = entries_.find(name);
		if (it == entries_.end()) return 0;
		const Entry& e = it->second;
		return cache::IsFresh(e.scope, e.stamp, e.ttl, clk::Now()) ? e.address : 0;
	}

	void Cache::Set(std::string_view name, std::uintptr_t address, cache::Scope scope, double ttlSeconds)
	{
		Store(name, address, scope, ttlSeconds);
	}

	void Cache::Invalidate(std::string_view name)
	{
		std::lock_guard lock(mutex_);
		const auto it = entries_.find(name);
		if (it == entries_.end() || !it->second.address) return;
		it->second.address = 0;
		stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
	}

	void Cache::InvalidateAll()
	{
		std::lock_guard lock(mutex_);
		for (auto& [name, e] : entries_)
		{
			if (!e.address) continue;
			e.address = 0;
			stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
		}
	}

	std::vector<Cache::View> Cache::Snapshot()
	{
		std::lock_guard lock(mutex_);
		std::vector<View> out;
		out.reserve(entries_.size());
		for (const auto& [name, e] : entries_) out.push_back({name, e.address, e.scope, e.resolves});
		return out;
	}

	Cache& Game()
	{
		static Cache c{"Addresses"};
		return c;
	}
}
