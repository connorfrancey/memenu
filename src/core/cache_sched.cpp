#include "core/cache_sched.h"

#include <algorithm>
#include <tuple>

namespace cache
{
	int Scheduler::Add(Job job)
	{
		std::lock_guard lock(lock_);
		Entry e;
		e.id = nextId_++;
		e.job = std::move(job);
		jobs_.push_back(std::move(e));
		return jobs_.back().id;
	}

	void Scheduler::Remove(int id)
	{
		std::lock_guard lock(lock_);
		jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Entry& e) { return e.id == id; }), jobs_.end());
	}

	void Scheduler::Wake(const std::string& name)
	{
		std::lock_guard lock(lock_);
		for (Entry& e : jobs_)
			if (e.job.name == name) e.dueAt = 0.0;
	}

	int Scheduler::RunSlice(double budgetMs, double now, const std::function<double()>& clock)
	{
		const double start = clock();
		const auto spentMs = [&] { return (clock() - start) * 1000.0; };
		int ran = 0;
		std::vector<int> done;		// Each job at most once a slice
		for (;;) {
			const double left = budgetMs - spentMs();
			if (left <= 0.0) break;
			// The due job that ran least recently, then by priority
			std::function<bool(double)> run;
			int id = 0;
			{
				std::lock_guard lock(lock_);
				const Entry* best = nullptr;
				for (const Entry& e : jobs_) {
					if (std::find(done.begin(), done.end(), e.id) != done.end()) continue;
					if (!e.busy && e.dueAt > now) continue;
					if (!best) { best = &e; continue; }
					const auto key = [&](const Entry& x) { return std::make_tuple(x.lastTurn, x.job.priority); };
					if (key(e) < key(*best)) best = &e;
				}
				if (!best) break;
				run = best->job.run;
				id = best->id;
			}
			done.push_back(id);
			const double t0 = clock();
			const bool finished = run ? run(left) : true;
			const double ms = (clock() - t0) * 1000.0;
			++ran;
			std::lock_guard lock(lock_);
			for (Entry& e : jobs_)
				if (e.id == id) {
					e.lastTurn = ++turns_;
					++e.runs;
					e.lastMs = ms;
					e.totalMs += ms;
					e.busy = !finished;
					if (finished) {
						++e.completions;
						e.dueAt = now + e.job.interval;
					}
				}
		}
		return ran;
	}

	std::vector<Scheduler::View> Scheduler::Snapshot(double now) const
	{
		std::lock_guard lock(lock_);
		std::vector<View> out;
		for (const Entry& e : jobs_) out.push_back({e.job.name, e.runs, e.completions, e.lastMs, e.totalMs, e.busy || e.dueAt <= now, e.busy});
		return out;
	}

	Scheduler& Maintenance()
	{
		static Scheduler* s = new Scheduler;	// Never destroyed, like the evictor
		return *s;
	}
}
