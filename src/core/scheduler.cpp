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

// ---- Public API (the state above lives in sched's anonymous namespace) -------------------------------------------------
namespace sched
{
	namespace
	{
		std::atomic<std::uint64_t> g_tickCount{0};

		float EffectiveInterval(const Entry& e)
		{
			return static_cast<float>(e.schedule.interval * Scale(e.schedule.priority));
		}

		void RecordRun(Entry& e, float us)
		{
			e.runs.fetch_add(1, std::memory_order_relaxed);
			e.lastUs.store(us, std::memory_order_relaxed);
			Ema(e.avgUs, us);
			Max(e.maxUs, us);
		}

		// Tasks added with AddTask since the last tick join the run order. Game thread.
		void AdoptPending()
		{
			std::lock_guard lock(g_mutex);
			if (g_pending.empty()) return;
			for (auto& e : g_pending) g_entries.push_back(std::move(e));
			g_pending.clear();
			BuildOrder();
		}

		void PushHistory(float workMs, float intervalMs)
		{
			const std::size_t pos = g_historyPos.load(std::memory_order_relaxed);
			g_workHistory[pos % kHistory].store(workMs, std::memory_order_relaxed);
			g_intervalHistory[pos % kHistory].store(intervalMs, std::memory_order_relaxed);
			g_historyPos.store(pos + 1, std::memory_order_release);
		}
	}

	void AddTask(std::string name, Schedule schedule, std::function<void()> run, std::function<bool()> needsUpdate)
	{
		auto e = std::make_unique<Entry>();
		e->name = std::move(name);
		e->schedule = std::move(schedule);
		e->run = std::move(run);
		e->needsUpdate = std::move(needsUpdate);
		e->effectiveInterval.store(e->schedule.interval, std::memory_order_relaxed);
		std::lock_guard lock(g_mutex);
		g_pending.push_back(std::move(e));
	}

	void Init()
	{
		if (g_initialized.load(std::memory_order_acquire)) return;
		{
			std::lock_guard lock(g_mutex);
			modules::ForEach([](modules::Tab&, Module& m) {
				auto e = std::make_unique<Entry>();
				e->name = m.Name();
				e->module = &m;
				e->schedule = m.schedule;
				e->effectiveInterval.store(m.schedule.interval, std::memory_order_relaxed);
				g_byModule.emplace(&m, e.get());
				g_entries.push_back(std::move(e));
			});
			for (auto& e : g_pending) g_entries.push_back(std::move(e));
			g_pending.clear();
			BuildOrder();
		}
		g_initialized.store(true, std::memory_order_release);
		spdlog::info("Scheduler: {} entries", g_order.size());
	}

	void Tick()
	{
		if (!g_initialized.load(std::memory_order_acquire)) return;
		cache::NextTick();
		AdoptPending();

		const clk::Ticks start = clk::Now();
		const float intervalMs = g_lastTickStart ? static_cast<float>(clk::Seconds(start - g_lastTickStart) * 1000.0) : 0.f;
		g_lastTickStart = start;
		const clk::Ticks budget = clk::FromSeconds(g_budgetMs.load(std::memory_order_relaxed) / 1000.0);

		for (int index : g_order)
		{
			Entry& e = *g_entries[static_cast<std::size_t>(index)];

			if (e.module && !e.module->Enabled())
			{
				e.state.store(State::Disabled, std::memory_order_relaxed);
				continue;
			}
			const bool blocked = std::any_of(e.requiredOn.begin(), e.requiredOn.end(), [](int dep) {
				const Module* m = g_entries[static_cast<std::size_t>(dep)]->module;
				return m && !m->Enabled();
			});
			if (blocked)
			{
				e.state.store(State::Blocked, std::memory_order_relaxed);
				continue;
			}

			const bool dirty = e.module && e.module->TakeDirty();
			const float interval = EffectiveInterval(e);
			e.effectiveInterval.store(interval, std::memory_order_relaxed);

			const clk::Ticks now = clk::Now();
			const double since = e.lastRun ? clk::Seconds(now - e.lastRun) : 1e9;
			const bool due = interval <= 0.f || since >= interval;
			if (!due && !dirty)
			{
				e.state.store(State::NotDue, std::memory_order_relaxed);
				e.skips.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			if (!dirty)
			{
				const bool needed = e.module ? e.module->NeedsUpdate() : (!e.needsUpdate || e.needsUpdate());
				if (!needed)
				{
					e.lastRun = now;
					e.state.store(State::Clean, std::memory_order_relaxed);
					e.skips.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
			}

			if (Deferrable(e.schedule.priority) && now - start > budget)
			{
				const bool starving = since >= std::max(4.0 * interval, kMinStarve);
				if (!starving)
				{
					e.state.store(State::Deferred, std::memory_order_relaxed);
					e.deferrals.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
			}

			try
			{
				if (e.module) e.module->OnTick();
				else if (e.run) e.run();
				e.state.store(State::Ran, std::memory_order_relaxed);
			}
			catch (const std::exception& ex)
			{
				spdlog::error("Scheduler: '{}' threw: {}", e.name, ex.what());
				e.state.store(State::Failed, std::memory_order_relaxed);
			}
			catch (...)
			{
				spdlog::error("Scheduler: '{}' threw an unknown exception", e.name);
				e.state.store(State::Failed, std::memory_order_relaxed);
			}
			e.lastRun = now;
			RecordRun(e, static_cast<float>(clk::Micros(clk::Now() - now)));
		}

		// Adapt: stretch Low/Background intervals while over budget, relax them again once there is headroom.
		const clk::Ticks work = clk::Now() - start;
		const float workMs = static_cast<float>(clk::Seconds(work) * 1000.0);
		g_lastMs.store(workMs, std::memory_order_relaxed);
		Ema(g_avgMs, workMs);
		double pressure = g_pressure.load(std::memory_order_relaxed);
		if (work > budget) pressure = std::min(kMaxPressure, pressure * 1.05);
		else if (work * 2 < budget) pressure = std::max(1.0, pressure / 1.02);
		g_pressure.store(pressure, std::memory_order_relaxed);

		PushHistory(workMs, intervalMs);
		g_tickCount.fetch_add(1, std::memory_order_relaxed);
	}

	void SetBudget(double milliseconds) { g_budgetMs.store(std::max(0.01, milliseconds), std::memory_order_relaxed); }
	double Budget() { return g_budgetMs.load(std::memory_order_relaxed); }

	std::vector<EntryView> Snapshot()
	{
		std::lock_guard lock(g_mutex);
		std::vector<EntryView> out;
		out.reserve(g_entries.size());
		for (const auto& p : g_entries)
		{
			const Entry& e = *p;
			EntryView v;
			v.name = e.name;
			v.isModule = e.module != nullptr;
			v.priority = e.schedule.priority;
			v.state = e.state.load(std::memory_order_relaxed);
			v.interval = e.schedule.interval;
			v.effectiveInterval = e.effectiveInterval.load(std::memory_order_relaxed);
			v.runs = e.runs.load(std::memory_order_relaxed);
			v.deferrals = e.deferrals.load(std::memory_order_relaxed);
			v.avgUs = e.avgUs.load(std::memory_order_relaxed);
			v.maxUs = e.maxUs.load(std::memory_order_relaxed);
			v.lastUs = e.lastUs.load(std::memory_order_relaxed);
			v.renderAvgUs = e.renderAvgUs.load(std::memory_order_relaxed);
			v.renderMaxUs = e.renderMaxUs.load(std::memory_order_relaxed);
			v.order = e.order;
			out.push_back(std::move(v));
		}
		return out;
	}

	TickView Ticks()
	{
		TickView t;
		t.pressure = g_pressure.load(std::memory_order_relaxed);
		t.lastMs = g_lastMs.load(std::memory_order_relaxed);
		t.avgMs = g_avgMs.load(std::memory_order_relaxed);
		t.budgetMs = static_cast<float>(g_budgetMs.load(std::memory_order_relaxed));
		t.ticks = g_tickCount.load(std::memory_order_relaxed);
		return t;
	}

	void History(std::vector<float>& workMs, std::vector<float>& intervalMs)
	{
		const std::size_t end = g_historyPos.load(std::memory_order_acquire);
		const std::size_t count = std::min(end, kHistory);
		workMs.resize(count);
		intervalMs.resize(count);
		for (std::size_t i = 0; i < count; ++i)
		{
			const std::size_t slot = (end - count + i) % kHistory;
			workMs[i] = g_workHistory[slot].load(std::memory_order_relaxed);
			intervalMs[i] = g_intervalHistory[slot].load(std::memory_order_relaxed);
		}
	}

	void RecordRender(const Module& module, double microseconds)
	{
		if (!g_initialized.load(std::memory_order_acquire)) return;
		const auto it = g_byModule.find(&module);
		if (it == g_byModule.end()) return;
		Ema(it->second->renderAvgUs, static_cast<float>(microseconds));
		Max(it->second->renderMaxUs, static_cast<float>(microseconds));
	}

	const char* PriorityName(Priority p)
	{
		switch (p)
		{
			case Priority::Critical: return "critical";
			case Priority::High: return "high";
			case Priority::Normal: return "normal";
			case Priority::Low: return "low";
			case Priority::Background: return "background";
		}
		return "?";
	}

	const char* StateName(State s)
	{
		switch (s)
		{
			case State::Idle: return "idle";
			case State::Ran: return "ran";
			case State::NotDue: return "not due";
			case State::Clean: return "clean";
			case State::Deferred: return "deferred";
			case State::Blocked: return "blocked";
			case State::Disabled: return "off";
			case State::Failed: return "failed";
		}
		return "?";
	}
}
