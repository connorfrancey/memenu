#include "game/object_query.h"

#include "core/cache.h"
#include "core/cache_evict.h"
#include "core/cache_sched.h"
#include "core/clock.h"
#include "core/scheduler.h"
#include "game/game.h"
#include "game/object_ref.h"
#include "game/uobject.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace game
{
	namespace
	{
		using Slots = std::vector<std::uint32_t>;

		struct Index
		{
			std::unordered_map<Classes::UClass*, Slots> byClass;
			std::unordered_map<std::int32_t, Slots> byName;		// FName index (the base name, any number)
			std::size_t scanned = 0;							// Slots [0, scanned) are in it
			std::uint32_t levelEpoch = 0;
			bool complete = false;

			void Add(std::uint32_t slot, Classes::UObject* o)
			{
				byClass[o->Class].push_back(slot);
				byName[o->Name.Index].push_back(slot);
			}
			std::size_t Bytes() const
			{
				std::size_t n = 0;
				for (const auto& [k, v] : byClass) n += v.size() * 4 + 48;
				for (const auto& [k, v] : byName) n += v.size() * 4 + 48;
				return n;
			}
		};

		// The index in use, and the one being built in slices
		struct State : cache::Evictor::Member
		{
			Index current, next;
			QueryStats stats;
			double passTicks = 0.0;		// Scanning time of the pass being built (seconds)

			State() : cache::Evictor::Member("Object index") {}
			std::size_t Bytes() const override { return current.Bytes() + next.Bytes(); }
			std::size_t Trim(std::size_t) override	// Rebuilt by the next pass
			{
				const std::size_t n = Bytes();
				current = Index{};
				next = Index{};
				return n;
			}
		};
		State& S()
		{
			static State* s = new State;
			return *s;
		}

		const auto& Objects() { return Classes::UObject::GetGlobalObjects(); }

		// Scans slots into `index` until `budgetMs` is spent (0: to the end). True when it got to the end.
		bool Scan(Index& index, double budgetMs)
		{
			const auto& objects = Objects();
			const clk::Ticks start = clk::Now(), limit = clk::FromSeconds(budgetMs / 1000.0);
			const std::size_t n = objects.Num();
			while (index.scanned < n) {
				const std::size_t end = std::min(n, index.scanned + 512);		// The clock is read every 512 slots
				for (std::size_t i = index.scanned; i < end; ++i)
					if (Classes::UObject* o = objects.GetByIndex(i); o && o->Class) index.Add(static_cast<std::uint32_t>(i), o);
				index.scanned = end;
				if (budgetMs > 0.0 && clk::Now() - start >= limit) break;
			}
			S().passTicks += clk::Seconds(clk::Now() - start);
			return index.scanned >= n;
		}

		void Finish(Index& built)
		{
			State& s = S();
			built.complete = true;
			s.current = std::move(built);
			s.next = Index{};
			++s.stats.passes;
			s.stats.lastPassMs = s.passTicks * 1000.0;
			s.passTicks = 0.0;
		}

		// One slice of the next pass (the maintenance job). True when a pass was completed.
		bool Step(double budgetMs)
		{
			State& s = S();
			if (s.next.scanned == 0) s.next.levelEpoch = cache::LevelEpoch();
			if (s.next.levelEpoch != cache::LevelEpoch()) {	// A level load meanwhile: start over
				s.next = Index{};
				s.passTicks = 0.0;
				++s.stats.restarts;
				return false;
			}
			if (!Scan(s.next, budgetMs)) return false;
			Finish(s.next);
			return true;
		}

		// The index to answer from: rebuilt now if asked to, or if the one there is from before a level load
		Index& Current(Freshness freshness)
		{
			State& s = S();
			if (freshness == Freshness::Exact || !s.current.complete || s.current.levelEpoch != cache::LevelEpoch()) {
				Index fresh;
				fresh.levelEpoch = cache::LevelEpoch();
				s.passTicks = 0.0;
				Scan(fresh, 0.0);
				Finish(fresh);			// Also drops a sliced pass under way: this one is newer
				++s.stats.exactPasses;
			}
			return s.current;
		}

		// The object still in `slot`, of class `cls`
		Classes::UObject* At(std::uint32_t slot, Classes::UClass* cls)
		{
			const auto& objects = Objects();
			if (slot >= objects.Num()) return nullptr;
			Classes::UObject* o = objects.GetByIndex(slot);
			return o && o->Class == cls ? o : nullptr;
		}

		bool ClassIsA(const Classes::UStruct* c, const Classes::UClass* base)
		{
			for (int depth = 0; c && depth < 64; c = SuperOf(c), ++depth)
				if (c == base) return true;
			return false;
		}
	}

	std::vector<Classes::UObject*> ObjectsOf(Classes::UClass* cls, Freshness freshness)
	{
		std::vector<Classes::UObject*> out;
		if (!cls) return out;
		const Index& index = Current(freshness);
		for (const auto& [c, slots] : index.byClass) {
			if (!ClassIsA(c, cls)) continue;
			for (const std::uint32_t slot : slots)
				if (Classes::UObject* o = At(slot, c)) out.push_back(o);
		}
		// Slots added since the index was built
		const auto& objects = Objects();
		for (std::size_t i = index.scanned; i < objects.Num(); ++i)
			if (Classes::UObject* o = objects.GetByIndex(i); o && o->Class && ClassIsA(o->Class, cls)) out.push_back(o);
		return out;
	}

	Classes::UObject* FindByPath(std::string_view path, Freshness freshness)
	{
		if (path.empty()) return nullptr;
		const std::size_t dot = path.find_last_of('.');
		std::string leaf(dot == std::string_view::npos ? path : path.substr(dot + 1));
		// "Name_3" is the name "Name" with the instance number 4 in the engine: its index is the base name's
		int name = NameIndex(leaf.c_str());
		if (name < 0) {
			const std::size_t us = leaf.find_last_of('_');
			if (us != std::string::npos && us + 1 < leaf.size() &&
				std::all_of(leaf.begin() + static_cast<std::ptrdiff_t>(us) + 1, leaf.end(), [](unsigned char c) { return std::isdigit(c); }))
				name = NameIndex(leaf.substr(0, us).c_str());
		}
		if (name < 0) return nullptr;
		const Index& index = Current(freshness);
		const auto matches = [&](Classes::UObject* o) { return o && o->Name.Index == name && o->GetPathName() == path; };
		if (const auto it = index.byName.find(name); it != index.byName.end()) {
			const auto& objects = Objects();
			for (const std::uint32_t slot : it->second)
				if (slot < objects.Num())
					if (Classes::UObject* o = objects.GetByIndex(slot); matches(o)) return o;
		}
		const auto& objects = Objects();
		for (std::size_t i = index.scanned; i < objects.Num(); ++i)
			if (Classes::UObject* o = objects.GetByIndex(i); matches(o)) return o;
		return nullptr;
	}

	namespace
	{
		// ULevel::Actors (a TTransArray: data, count, max, owner) right after UObject
		struct RawActors
		{
			Classes::AActor** data;
			std::int32_t count, max;
		};
		constexpr std::size_t kLevelActors = 0x3C;

		const RawActors* ActorsIn(Classes::UObject* level)
		{
			if (!level) return nullptr;
			const auto* a = reinterpret_cast<const RawActors*>(reinterpret_cast<const std::uint8_t*>(level) + kLevelActors);
			if (!a->data || a->count <= 0 || a->count > 1'000'000 || a->max < a->count) return nullptr;
			return a;
		}

		struct ActorCache
		{
			std::uint32_t tick = ~0u;
			std::vector<Classes::AActor*> all;		// Every actor of the levels in play, this tick
			std::unordered_map<Classes::UClass*, std::vector<Classes::AActor*>> byClass;
		};
		ActorCache& Actors()
		{
			static ActorCache* c = new ActorCache;
			return *c;
		}

		// Every actor in play, from the levels' lists (false: they don't look right, use the index)
		bool CollectActors(std::vector<Classes::AActor*>& out, std::size_t& levels)
		{
			auto* world = static_cast<Classes::AWorldInfo*>(World());
			if (!world) return false;
			const RawActors* persistent = ActorsIn(world->Outer);
			if (!persistent || persistent->data[0] != world) return false;		// UE3: the WorldInfo is the first
			std::vector<const RawActors*> lists{persistent};
			for (std::size_t i = 0; i < world->StreamingLevels.Num(); ++i) {
				Classes::ULevelStreaming* s = world->StreamingLevels[i];
				if (s && s->bIsVisible && s->LoadedLevel)
					if (const RawActors* a = ActorsIn(s->LoadedLevel)) lists.push_back(a);
			}
			levels = lists.size();
			for (const RawActors* a : lists)
				for (std::int32_t i = 0; i < a->count; ++i)
					if (Classes::AActor* actor = a->data[i]) out.push_back(actor);
			return true;
		}
	}

	std::vector<Classes::AActor*> ActorsOf(Classes::UClass* cls)
	{
		if (!cls) return {};
		ActorCache& c = Actors();
		State& s = S();
		if (c.tick != cache::TickEpoch()) {
			c.tick = cache::TickEpoch();
			c.all.clear();
			c.byClass.clear();
			std::size_t levels = 0;
			s.stats.levelListsOk = CollectActors(c.all, levels);
			s.stats.levels = levels;
			if (!s.stats.levelListsOk) {
				for (Classes::UObject* o : ObjectsOf(Classes::AActor::StaticClass()))
					if (o->GetName().rfind("Default__", 0) != 0) c.all.push_back(static_cast<Classes::AActor*>(o));
			}
			s.stats.actors = c.all.size();
		}
		auto [it, added] = c.byClass.try_emplace(cls);
		if (added)
			for (Classes::AActor* a : c.all)
				if (a->Class && ClassIsA(a->Class, cls)) it->second.push_back(a);
		return it->second;
	}

	QueryStats GetQueryStats()
	{
		State& s = S();
		QueryStats out = s.stats;
		out.slots = s.current.scanned;
		out.classes = s.current.byClass.size();
		out.names = s.current.byName.size();
		return out;
	}

	void InitObjectQueries()
	{
		// Every 30 s: queries check every object and scan the newest slots, and a level load rebuilds it; the pass only has
		// to catch objects made in reused slots meanwhile
		cache::Maintenance().Add({"Object index", 30.0, 1, [](double budgetMs) { return Step(budgetMs); }});
		cache::Maintenance().Add({"Eviction", 1.0, 5, [](double) {
			cache::Evictions().Enforce();
			return true;
		}});
		// The caches' upkeep: a small slice of each tick, deferred when the tick is busy
		sched::AddTask("Cache maintenance", {0.f, sched::Priority::Background, {}, {}}, [] {
			cache::Maintenance().RunSlice(0.25, clk::Seconds(clk::Now()), [] { return clk::Seconds(clk::Now()); });
		});
	}
}
