#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// The cache scheduler: the caches' upkeep, spread over engine ticks within a small time slice, so that it never shows
// as a hitch. Jobs are things like keeping the object index current (game/object_query.h), dropping expired entries
// and the eviction pass (core/cache_evict.h), and warming caches after a level load.
//
//   cache::Maintenance().Add({"Object index", 0.0, 0, [](double budgetMs) { return index.Step(budgetMs); }});
//
// Each RunSlice(budgetMs): the due jobs (and those in the middle of their work) run in turn, the one that ran least
// recently first, then by priority, each told how much of the slice is left; a job does a bounded piece of its work and
// returns true when it is done for now (then it waits its interval) or false when it has more (it goes on in a later
// slice). Taking turns, a long job can't keep the others out. Wake() makes a job due now (a level load starts the
// index over).
//
// The menu runs it as one Background task of the tick scheduler (core/scheduler.h): deferred when the tick is busy,
// never left behind for long. Game thread; Add/Remove/Wake/Snapshot from any thread.
namespace cache
{
	struct Job
	{
		std::string name;
		double interval = 0.0;		// Seconds to wait after it reported done (0: due again at once, while there's time)
		int priority = 0;			// Lower first, among jobs equally overdue
		std::function<bool(double budgetMs)> run;
	};

	class Scheduler
	{
	public:
		int Add(Job job);			// Its id; due at once
		void Remove(int id);
		void Wake(const std::string& name);

		// Runs due jobs until `budgetMs` (measured with `clock`, in seconds) is spent. Returns the jobs that ran.
		int RunSlice(double budgetMs, double now, const std::function<double()>& clock);

		struct View
		{
			std::string name;
			std::uint64_t runs = 0, completions = 0;
			double lastMs = 0.0, totalMs = 0.0;
			bool due = false, busy = false;		// busy: in the middle of its work
		};
		std::vector<View> Snapshot(double now) const;

	private:
		struct Entry
		{
			int id;
			Job job;
			double dueAt = 0.0;
			bool busy = false;
			std::uint64_t lastTurn = 0;		// When it last ran (a counter): the least recent goes first
			std::uint64_t runs = 0, completions = 0;
			double lastMs = 0.0, totalMs = 0.0;
		};
		mutable std::mutex lock_;
		std::vector<Entry> jobs_;
		int nextId_ = 1;
		std::uint64_t turns_ = 0;
	};

	Scheduler& Maintenance();		// The menu's
}
