#include "runtime/resolve.h"

#include <algorithm>
#include <functional>
#include <set>

namespace mirage::mods
{
	Resolution Resolve(const std::vector<Manifest>& mods, const semver::Version& runtime,
					   const std::map<std::string, std::string>& off)
	{
		Resolution r;
		std::map<std::string, const Manifest*> byId;
		for (const Manifest& m : mods) {
			if (!byId.emplace(m.id, &m).second) r.excluded[m.id] = "two mods have the id '" + m.id + "'";
		}
		const auto exclude = [&](const std::string& id, std::string why) {
			if (!r.excluded.count(id)) r.excluded[id] = std::move(why);
		};

		// What each mod is on its own
		for (const auto& [id, m] : byId) {
			if (r.excluded.count(id)) continue;
			if (const auto it = off.find(id); it != off.end()) exclude(id, it->second);
			else if (!m->mirage.Matches(runtime))
				exclude(id, "it needs Mirage " + m->mirage.Text() + ", this is " + runtime.ToString());
		}
		for (const auto& [id, m] : byId) {
			if (r.excluded.count(id)) continue;
			for (const Dependency& d : m->incompatible)
				if (const auto it = byId.find(d.id); it != byId.end() && !off.count(d.id) && d.range.Matches(it->second->version)) {
					exclude(id, "it doesn't work with " + d.id + " " + it->second->version.ToString());
					break;
				}
		}
		for (const auto& [id, m] : byId) {
			if (r.excluded.count(id)) continue;
			for (const Dependency& d : m->required) {
				const auto it = byId.find(d.id);
				if (it == byId.end()) {
					exclude(id, "it needs " + d.id + " " + d.range.Text() + ", which isn't installed");
					break;
				}
				if (!d.range.Matches(it->second->version)) {
					exclude(id, "it needs " + d.id + " " + d.range.Text() + ", " + it->second->version.ToString() + " is installed");
					break;
				}
				if (static_cast<int>(it->second->phase) > static_cast<int>(m->phase)) {
					exclude(id, std::string("it loads in the ") + PhaseName(m->phase) + " phase but needs " + d.id +
									", which loads later (" + PhaseName(it->second->phase) + ")");
					break;
				}
			}
		}

		// Cycles of required mods: every mod on one is left out
		{
			std::map<std::string, int> state;	// 0 new, 1 on the path, 2 done
			std::vector<std::string> path;
			std::function<void(const std::string&)> visit = [&](const std::string& id) {
				state[id] = 1;
				path.push_back(id);
				for (const Dependency& d : byId.at(id)->required) {
					if (!byId.count(d.id)) continue;
					if (state[d.id] == 1) {
						const auto from = std::find(path.begin(), path.end(), d.id);
						std::string loop;
						for (auto it = from; it != path.end(); ++it) loop += *it + " -> ";
						loop += d.id;
						for (auto it = from; it != path.end(); ++it) exclude(*it, "its required mods form a cycle: " + loop);
					} else if (state[d.id] == 0) {
						visit(d.id);
					}
				}
				path.pop_back();
				state[id] = 2;
			};
			for (const auto& [id, m] : byId)
				if (state[id] == 0) visit(id);
		}

		// Left out spreads to the mods that require it (until nothing changes)
		for (bool changed = true; changed;) {
			changed = false;
			for (const auto& [id, m] : byId) {
				if (r.excluded.count(id)) continue;
				for (const Dependency& d : m->required)
					if (const auto it = r.excluded.find(d.id); it != r.excluded.end()) {
						exclude(id, "it needs " + d.id + ", which isn't loaded (" + it->second + ")");
						changed = true;
						break;
					}
			}
		}

		// Order: Kahn's algorithm over required and usable optional edges; the next mod is the one with the earliest
		// phase, then the lowest id
		std::map<std::string, std::set<std::string>> before;	// id -> mods that must load before it
		for (const auto& [id, m] : byId) {
			if (r.excluded.count(id)) continue;
			auto& deps = before[id];
			for (const Dependency& d : m->required) deps.insert(d.id);
			for (const Dependency& d : m->optional) {
				const auto it = byId.find(d.id);
				if (it == byId.end()) continue;		// Not installed: fine, that's what optional means
				if (r.excluded.count(d.id)) {
					r.warnings.push_back(id + ": optional " + d.id + " isn't loaded (" + r.excluded.at(d.id) + ")");
					continue;
				}
				if (!d.range.Matches(it->second->version)) {
					r.warnings.push_back(id + ": optional " + d.id + " " + d.range.Text() + " is " +
										 it->second->version.ToString() + ": not used for the order");
					continue;
				}
				if (static_cast<int>(it->second->phase) > static_cast<int>(m->phase)) {
					r.warnings.push_back(id + ": optional " + d.id + " loads in a later phase: not used for the order");
					continue;
				}
				deps.insert(d.id);
			}
		}
		std::set<std::string> done;
		while (done.size() < before.size()) {
			const auto pick = [&](bool optionalBroken) -> std::string {
				std::string best;
				for (const auto& [id, deps] : before) {
					if (done.count(id)) continue;
					bool ready = true;
					for (const std::string& d : deps)
						if (!done.count(d)) {
							// Breaking a cycle: only required edges still count
							const auto& req = byId.at(id)->required;
							const bool required = std::any_of(req.begin(), req.end(), [&](const Dependency& x) { return x.id == d; });
							if (!optionalBroken || required) ready = false;
						}
					if (!ready) continue;
					const auto rank = [&](const std::string& x) { return std::make_pair(static_cast<int>(byId.at(x)->phase), x); };
					if (best.empty() || rank(id) < rank(best)) best = id;
				}
				return best;
			};
			std::string next = pick(false);
			if (next.empty()) {
				next = pick(true);
				if (next.empty()) break;	// Can't happen: required cycles were left out above
				r.warnings.push_back(next + ": a cycle through optional dependencies was broken here");
			}
			done.insert(next);
			r.order.push_back(next);
		}
		return r;
	}
}
