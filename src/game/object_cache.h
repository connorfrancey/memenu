#pragma once
#include "core/cache.h"
#include "core/cache_evict.h"
#include "core/negative_cache.h"
#include "core/string_cache.h"

#include "sdk/SDK.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

// Game objects by full name ("Class TdGame.TdPlayerPawn", "Function TdGame.TdPlayerController.PlayerTick"), found once
// and reused while they live. Game thread only.
//
//   auto* fn = game::FindObject<Classes::UFunction>("Function TdGame.TdHUD.PostRender");
//
// A found object is only trusted while its GObjects slot still holds that very pointer (the check reads the slot, not
// the object: no freed memory is touched), like game::ObjectRef. A name that isn't found is looked for again only after
// `retryTicks` engine ticks (doubling for each miss in a row, up to 8x) or a level load: a lookup that can't succeed on
// this screen doesn't walk ~100k objects on every call. Names are interned (str::Intern), so the map holds no copies.
// For objects found by a test rather than a name, use game::ObjectRef.
// Without optimizations (MEMENU_OPTIMIZATIONS off) every Find() asks the SDK again.
namespace game
{
	// Under the eviction manager (core/cache_evict.h): trimmed, it forgets what it found (found again when asked).
	class ObjectCache : public cache::Evictor::Member
	{
	public:
		explicit ObjectCache(std::string name, std::uint32_t retryTicks = 120);

		std::size_t Bytes() const override { return found_.size() * (sizeof(Entry) + 6 * sizeof(void*)); }
		std::size_t Trim(std::size_t) override
		{
			const std::size_t n = Bytes();
			Clear();
			return n;
		}
		std::uint64_t Hits() const override { return stats_->hits.load(std::memory_order_relaxed); }

		Classes::UObject* Find(std::string_view fullName);
		template <class T>
		T* Find(std::string_view fullName) { return static_cast<T*>(Find(fullName)); }

		void Forget(std::string_view fullName);
		void Clear();
		std::size_t Size() const { return found_.size(); }

	private:
		struct Entry
		{
			Classes::UObject* object;
			std::size_t index;		// Its GObjects slot
		};

		cache::Stats* stats_;
		std::unordered_map<str::Atom, Entry> found_;
		cache::NegativeCache<str::Atom> missing_;
	};

	// The shared cache (game thread).
	ObjectCache& Objects();
	template <class T>
	T* FindObject(std::string_view fullName) { return Objects().Find<T>(fullName); }
}
