#include "core/string_cache.h"
#include "core/cache.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace str
{
	namespace
	{
		// Interned text lives in blocks that are never freed or moved: each string is [uint32 length][text][\0].
		// The map's string_view keys point into the blocks, so a lookup by string_view needs no allocation.
		constexpr std::size_t kBlock = 64 * 1024;

		struct Table
		{
			std::shared_mutex mutex;
			std::unordered_map<std::string_view, const char*> index;
			std::vector<std::unique_ptr<char[]>> blocks;
			std::size_t used = kBlock;		// In the last block (full: none yet)
			std::size_t bytes = 0;
			std::uint64_t added = 0;
			std::atomic<std::uint64_t> lookups{0};
			cache::Stats* stats = &cache::Register("Interned strings", cache::Scope::Manual);

			// Under the exclusive lock.
			const char* Store(std::string_view text)
			{
				const std::size_t need = sizeof(std::uint32_t) + text.size() + 1;
				char* at;
				if (need > kBlock / 4)		// Long strings get a block of their own
				{
					blocks.push_back(std::make_unique<char[]>(need));
					at = blocks.back().get();
					// Keep filling the previous block afterwards: move the big one before it
					if (blocks.size() > 1) std::swap(blocks.back(), blocks[blocks.size() - 2]);
				}
				else
				{
					if (used + need > kBlock)
					{
						blocks.push_back(std::make_unique<char[]>(kBlock));
						used = 0;
					}
					at = blocks.back().get() + used;
					used += need;
				}
				const auto len = static_cast<std::uint32_t>(text.size());
				std::memcpy(at, &len, sizeof len);
				char* textAt = at + sizeof len;
				std::memcpy(textAt, text.data(), text.size());
				textAt[text.size()] = '\0';
				bytes += need;
				++added;
				return textAt;
			}
		};

		Table& GetTable()
		{
			static Table* t = new Table;	// Never destroyed: Atoms may be used during static destruction
			return *t;
		}
	}

	std::string_view Atom::view() const
	{
		if (!text_) return {};
		std::uint32_t len;
		std::memcpy(&len, text_ - sizeof len, sizeof len);
		return {text_, len};
	}

	Atom Intern(std::string_view text)
	{
		if (text.empty()) return Atom{};
		Table& t = GetTable();
		t.lookups.fetch_add(1, std::memory_order_relaxed);
		{
			std::shared_lock lock(t.mutex);
			if (auto it = t.index.find(text); it != t.index.end())
			{
				t.stats->hits.fetch_add(1, std::memory_order_relaxed);
				return Atom{it->second};
			}
		}
		std::unique_lock lock(t.mutex);
		if (auto it = t.index.find(text); it != t.index.end()) return Atom{it->second};		// Added meanwhile
		const char* stored = t.Store(text);
		t.index.emplace(std::string_view(stored, text.size()), stored);
		t.stats->misses.fetch_add(1, std::memory_order_relaxed);
		t.stats->entries.store(t.index.size(), std::memory_order_relaxed);
		return Atom{stored};
	}

	Atom Find(std::string_view text)
	{
		if (text.empty()) return Atom{};
		Table& t = GetTable();
		std::shared_lock lock(t.mutex);
		auto it = t.index.find(text);
		return it != t.index.end() ? Atom{it->second} : Atom{};
	}

	Stats GetStats()
	{
		Table& t = GetTable();
		std::shared_lock lock(t.mutex);
		return {t.index.size(), t.bytes, t.lookups.load(std::memory_order_relaxed), t.added};
	}
}
