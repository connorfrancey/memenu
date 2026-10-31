#include "core/scheduler.h"
#include "core/cache.h"
#include "core/clock.h"
#include "core/module.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sched 
{
	namespace 
	{
		constexpr double kMaxPressure = 8.0;
		constexpr double kMinStarve = 0.25;			// Seconds a deferred entry waits at most (or 4 intervals)
		
		struct Entry 
		{
			std::string name;
			Module* module = nullptr;				// or a task:
			std::function<void()> run;
			std::function<bool()> needsUpdate;
			Schedule schedule;
			
			// Resolved by BuildOrder (indices into g_entries)
			std::vector<int> requiredOn;
			int order = 0;
			
			// Game thread
			clk::Ticks lastRun = 0;					// 0 = never: due at once
			
			// Statistics: written by the game thread (render ones by the render thread), read by anyone
			std::atomic<State> state{State::Idle};
			std::atomic<std::uint64_t> runs{0}, deferrals{0}, skips{0};
			std::atomic<float> avgUs{0.f}, maxUs{0.f}, lastUs{0.f};
			std::atomic<float> renderAvgUs{0.f}, renderMaxUs{0.f};
			std::atomic<float> effectiveInterval{0.f};
		};
		
		std::mutex g_mutex;								// Structure of g_entries / g_pending (not the stats)
		std::vector<std::unique_ptr<Entry>> g_entries;
		std::vector<std::unique_ptr<Entry>> g_pending;	// Tasks added since the last tick
		std::vector<int> g_order;						// Indices into g_entries, run order (game thread)
		std::unordered_map<const Module*, Entry*> g_byModule;	// Built once by Init, read-only afterwards
		std::atomic<bool> g_initialized{false};			// RenderRecord (render thread) checks it
		
		std::atomic<double> g_budgetMs{1.5};
		std::atomic<double> g_pressure{1.0};
		std::atomic<float> g_lastMs{0.f}, g_avgMs{0.f};
		clk::Ticks g_lastTickStart = 0;

		// Fixed-size rings: no allocation while ticking
		std::array<std::atomic<float>, kHistory> g_workHistory{};
		std::array<std::atomic<float>, kHistory> g_intervalHistory{};
		std::atomic<std::size_t> g_historyPos{0};

		void Ema(std::atomic<float>& avg, float sample, float weight = 0.1f) 
		{
			const float a = avg.load(std::memory_order_relaxed);
			avg.store(a == 0.f ? sample : a + (sample - a) * weight, std::memory_order_relaxed);
		}
		
		void Max(std::atomic<float>& m, float sample) 
		{
			if (sample > m.load(std::memory_order_relaxed)) m.store(sample, std::memory_order_relaxed);
		}
		
		double Scale(Priority p) 
		{
			const double pressure = g_pressure.load(std::memory_order_relaxed);
			switch (p) 
			{
				case Priority::Low: return pressure;
				case Priority::Background: return pressure * pressure;
				default: return 1.0;
			}
		}
		
		bool Deferrable(Priority p) { return p == Priority::Low || p == Priority::Background; }
		
		// Topological order (Kahn) over `after`, picking the highest priority (then name) among the ready entries. Entries
		// caught in a cycle are appended by priority, with an error in the log.
		void BuildOrder()
		{
			const int n = static_cast<int>(g_entries.size());
			std::unordered_map<std::string, int> byName;
			for (int i = 0; i < n; ++i) byName.emplace(g_entries[i]->name, i);
			
			std::vector<std::vector<int>> next(n);
			std::vector<int> waiting(n, 0);
			for (int i = 0; i < n; ++i) 
			{
				Entry& e = *g_entries[i];
				for (const auto& dep : e.schedule.after) 
				{
					auto it = byName.find(dep);
					if (it == byName.end()) { spdlog::warn("Scheduler: '{}' runs after unknown '{}'", e.name, dep); continue; }
					if (it->second == i) continue;
					next[it->second].push_back(i);
					++waiting[i];
				}
				e.requiredOn.clear();
				for (const auto& dep : e.schedule.requiredOn) 
				{
					auto it = byName.find(dep);
					if (it == byName.end() || !g_entries[it->second]->module) 
					{
						spdlog::warn("Scheduler: '{}' requires unknown module '{}'", e.name, dep);
					}
					else 
					{
						e.requiredOn.push_back(it->second);
					}
				}
				
			}
			
			auto before = [](const Entry& a, const Entry& b) 
			{
				if (a.schedule.priority != b.schedule.priority) return a.schedule.priority < b.schedule.priority;
				return a.name < b.name;
			};

			
			std::vector<int> ready, order;
			std::vector<bool> placed(n, false);
			for (int i = 0; i < n; ++i) 
			{
				if (waiting[i] == 0) ready.push_back(i);
			}
			while (!ready.empty())
			{
				auto best = std::min_element(ready.begin(), ready.end(),
											[&](int a, int b) { return before(*g_entries[a], *g_entries[b]); });
				const int i = *best;
				ready.erase(best);
				order.push_back(i);
				placed[i] = true;
				for (int j : next[i]) 
				{
					if (--waiting[j] == 0) ready.push_back(j);
				}
			}
			if (static_cast<int>(order.size()) < n) 
			{
				std::vector<int> rest;
				for (int i = 0; i < n; ++i) 
				{
					
					if (!placed[i]) rest.push_back(i);
				}
				std::sort(rest.begin(), rest.end(), [&](int a, int b) { return before(*g_entries[a], *g_entries[b]); });
				std::string names;
				for (int i : rest) names += (names.empty() ? "" : ", ") + g_entries[i]->name;
				spdlog::error("Scheduler: dependency cycle between {} (run by priority instead)", names);
				order.insert(order.end(), rest.begin(), rest.end());
			}

			for (int k = 0; k < n; ++k) g_entries[order[k]]->order = k;
			g_order = std::move(order);
		}
	}
}