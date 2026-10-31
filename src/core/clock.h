#pragma once
#include <cstdint>

// A real-time clock for measuring the menu itself (scheduler, caches, performance monitor).
//
// It can't be QueryPerformanceCounter: during a TAS session the game thread's QPC returns a virtual clock. This uses the
// CPU's time stamp counter when it runs at a constant rate (every CPU of the last ~15 years), calibrated once, and the
// precise system time otherwise. Neither is hooked. Reading it costs a few nanoseconds.
namespace clk 
{
	using Ticks = std::uint64_t;
	
	void Init();					// Calibrate (~20ms). Call once at startup, before the game thread uses it
	Ticks Now();
	double Seconds(Ticks ticks);	// A duration in ticks -> seconds
	Ticks FromSeconds(double seconds);
	inline double SecondsSince(Ticks start) { return Seconds(Now() - start); }
	inline double Micros(Ticks ticks) { return Seconds(ticks) * 1e6; }
}