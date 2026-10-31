#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Module;

// Decides, every engine tick, which modules (and background tasks) run and in which order.
//
// Order: dependencies first (`after`), then priority, then name. Worked out once; a cycle is reported and broken.
// Each tick, for every enabled entry in that order:
//
//   blocked    a module named in `requiredOn` is off                     -> skipped
//   not due    `interval` (seconds, 0 = every tick) hasn't passed        -> skipped, unless marked dirty
//   clean      NeedsUpdate() says there's nothing to do                  -> skipped until it is due again
//   deferred   Low/Background work while this tick is over its budget    -> runs on a later tick
//   ran
//
// Adaptive: when the scheduled work keeps going over the budget, the intervals of Low and Background entries stretch
// (up to 8x and 64x) and shrink back once there is headroom again. A deferred entry runs anyway once it has waited
// four of its intervals (at least 250 ms), so nothing starves.
//
// Critical, High and Normal entries are never deferred or stretched. Anything that changes the game must be one of them
// with interval 0 (intervals are real time): then a TAS run does the same on every machine.
namespace sched 
{
	enum class Priority : std::uint8_t { Critical = 0, High, Normal, Low, Background };
	
	struct Schedule 
	{
		float interval = 0.f;		// Seconds between runs, 0 = every tick
		Priority priority = Priority::Normal;
		std::vector<std::string> after;			// Run after these modules / tasks in the same tick
		std::vector<std::string> requiredOn;	// Only run while these modules are on
	};
	
	enum class State : std::uint8_t { Idle, Ran, NotDue, Clean, Deferred, Blocked, Disabled, Failed };
	
	// Background work that isn't a module (list refreshes, ...). `needsUpdate` may be empty (always needs it).
	void AddTask(std::string name, Schedule schedule, std::function<void()> run, std::function<bool()> needsUpdate = {});
	
	void Init();		// After modules::Init(): builds the run order
	void Tick();		// Game thread, once per engine tick
	
	void SetBudget(double milliseconds);	// Per-tick time for Low/Background work (default 1.5 ms)
	double Budget();
	
	// ---- Statistics (any thread) ------------------------------------------------------------------------------------------
	struct EntryView 
	{
		std::string name;
		bool isModule = false;
		Priority priority = Priority::Normal;
		State state = State::Idle;
		float interval = 0.f, effectiveInterval = 0.f;
		std::uint64_t runs = 0, deferrals = 0;
		float avgUs = 0.f, maxUs = 0.f, lastUs = 0.f;			// OnTick / task cost
		float renderAvgUs = 0.f, renderMaxUs = 0.f;				// Modules: OnRender cost
		int order = 0;	
	};
	std::vector<EntryView> Snapshot();
	
	struct TickView 
	{
		double pressure = 1.0;				// Interval stretch of Low entries (Background: squared)
		float lastMs = 0.f, avgMs = 0.f;	// Scheduled work per tick
		float budgetMs = 0.f;
		std::uint64_t ticks = 0;
	};
	TickView Ticks();
	
	// The last `count` (<= kHistory) tick work times and tick intervals (ms), oldest first.
	constexpr std::size_t kHistory = 240;
	void History(std::vector<float>& workMs, std::vector<float>& intervalMs);
	
	// Modules mesure their OnRender with this (modules::Render does it).
	void RecordRender(const Module& module, double microseconds);
	
	const char* PriorityName(Priority p);
	const char* StateName(State s);
} 