#include "game/object_cache.h"

namespace game
{
	namespace
	{
		cache::Retry RetryPolicy(std::uint32_t ticks)
		{
			cache::Retry r;
			r.ticks = ticks;			// Ticks, not seconds: the same in every TAS run
			r.untilLevel = true;
			r.backoff = true;
			r.maxBackoff = 8;
			return r;
		}

		bool StillThere(const Classes::UObject* object, std::size_t index)
		{
			const auto& objects = Classes::UObject::GetGlobalObjects();
			return index < objects.Num() && objects.GetByIndex(index) == object;
		}
	}

	ObjectCache::ObjectCache(std::string name, std::uint32_t retryTicks)
		: cache::Evictor::Member(name), stats_(&cache::Register(name, cache::Scope::Level)),
		  missing_(name + " (not found)", RetryPolicy(retryTicks)) {}

	Classes::UObject* ObjectCache::Find(std::string_view fullName)
	{
		const str::Atom key = str::Intern(fullName);
		if (opt::kEnabled)
		{
			if (auto it = found_.find(key); it != found_.end())
			{
				if (StillThere(it->second.object, it->second.index))
				{
					stats_->hits.fetch_add(1, std::memory_order_relaxed);
					return it->second.object;
				}
				found_.erase(it);		// Destroyed (or its slot reused): look again
				stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			}
			if (missing_.Failed(key)) return nullptr;
		}

		Classes::UObject* object = cache::Compute(*stats_, [&] {
			return Classes::UObject::FindObject<Classes::UObject>(key.str());
		});
		if (!opt::kEnabled) return object;
		if (!object)
		{
			missing_.Remember(key);
			return nullptr;
		}
		missing_.Forget(key);
		const auto index = static_cast<std::size_t>(object->ObjectInternalInteger);
		if (StillThere(object, index)) found_[key] = {object, index};		// Else not trusted: found again next time
		stats_->entries.store(found_.size(), std::memory_order_relaxed);
		return object;
	}

	void ObjectCache::Forget(std::string_view fullName)
	{
		const str::Atom key = str::Find(fullName);
		if (key.empty()) return;
		if (found_.erase(key)) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
		missing_.Forget(key);
		stats_->entries.store(found_.size(), std::memory_order_relaxed);
	}

	void ObjectCache::Clear()
	{
		if (!found_.empty()) stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
		found_.clear();
		missing_.Clear();
		stats_->entries.store(0, std::memory_order_relaxed);
	}

	ObjectCache& Objects()
	{
		static ObjectCache c{"Objects by name"};
		return c;
	}
}
