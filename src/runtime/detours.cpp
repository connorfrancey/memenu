#include "runtime/detours.h"

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace mirage::detours
{
	namespace
	{
		struct Entry
		{
			int id;
			void* detour;
			int priority;
			int order;
			void** original;
			std::string owner;
		};
		struct Target
		{
			std::vector<Entry> chain;		// Highest priority first, then oldest
			void* trampoline = nullptr;		// MinHook's: the real function
		};

		std::mutex g_lock;
		std::map<void*, Target> g_targets;
		int g_nextId = 1, g_order = 0;

		std::string Describe(const Target& t)
		{
			std::string s;
			for (const Entry& e : t.chain) s += (s.empty() ? "" : " -> ") + e.owner + " (" + std::to_string(e.priority) + ")";
			return s;
		}

		// MinHook on `target` to the chain's first detour, every *original set: a detour's to the next, the last's to
		// the real function. "" when done.
		std::string Relink(void* target, Target& t)
		{
			// MinHook can't move a hook to another detour: the hook is made again (the trampoline changes with it)
			if (t.trampoline) {
				MH_DisableHook(target);
				MH_RemoveHook(target);
				t.trampoline = nullptr;
			}
			if (t.chain.empty()) return {};
			void* trampoline = nullptr;
			const MH_STATUS created = MH_CreateHook(target, t.chain.front().detour, &trampoline);
			if (created != MH_OK)
				return created == MH_ERROR_ALREADY_CREATED ? "the menu hooks this function itself" : MH_StatusToString(created);
			t.trampoline = trampoline;
			for (std::size_t i = 0; i < t.chain.size(); ++i)
				*t.chain[i].original = i + 1 < t.chain.size() ? t.chain[i + 1].detour : trampoline;
			const MH_STATUS enabled = MH_EnableHook(target);
			if (enabled != MH_OK) {
				MH_RemoveHook(target);
				t.trampoline = nullptr;
				return MH_StatusToString(enabled);
			}
			return {};
		}
	}

	int Add(void* target, void* detour, int priority, void** original, const std::string& owner, std::string* error)
	{
		if (!target || !detour || !original) {
			if (error) *error = "a target, a detour and a place for the original are needed";
			return 0;
		}
		std::lock_guard lock(g_lock);
		Target& t = g_targets[target];
		if (std::any_of(t.chain.begin(), t.chain.end(), [&](const Entry& e) { return e.detour == detour; })) {
			if (error) *error = "this detour is on the target already";
			return 0;
		}
		const Target before = t;
		const int id = g_nextId++;
		t.chain.push_back({id, detour, priority, g_order++, original, owner});
		std::stable_sort(t.chain.begin(), t.chain.end(), [](const Entry& a, const Entry& b) {
			return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
		});
		if (const std::string why = Relink(target, t); !why.empty()) {
			t.chain = before.chain;
			Relink(target, t);
			if (t.chain.empty()) g_targets.erase(target);
			if (error) *error = why;
			spdlog::error("Detours: {} can't detour {}: {}", owner, target, why);
			return 0;
		}
		if (t.chain.size() > 1)
			spdlog::warn("Detours: {} mods detour {}: {} (in that order)", t.chain.size(), target, Describe(t));
		else
			spdlog::info("Detours: {} detours {}", owner, target);
		return id;
	}

	void Remove(int id)
	{
		std::lock_guard lock(g_lock);
		for (auto it = g_targets.begin(); it != g_targets.end(); ++it) {
			Target& t = it->second;
			const auto e = std::find_if(t.chain.begin(), t.chain.end(), [&](const Entry& x) { return x.id == id; });
			if (e == t.chain.end()) continue;
			spdlog::info("Detours: {} removed its detour of {}", e->owner, it->first);
			*e->original = nullptr;
			t.chain.erase(e);
			if (const std::string why = Relink(it->first, t); !why.empty())
				spdlog::error("Detours: the chain on {} couldn't be made again: {}", it->first, why);
			if (t.chain.empty()) g_targets.erase(it);
			return;
		}
	}

	void RemoveAll()
	{
		std::lock_guard lock(g_lock);
		for (auto& [target, t] : g_targets) {
			t.chain.clear();
			Relink(target, t);
		}
		g_targets.clear();
	}

	std::vector<std::string> Chain(void* target)
	{
		std::lock_guard lock(g_lock);
		std::vector<std::string> out;
		if (const auto it = g_targets.find(target); it != g_targets.end())
			for (const Entry& e : it->second.chain) out.push_back(e.owner + " (" + std::to_string(e.priority) + ")");
		return out;
	}
}
