#include "clock.h"

#include <Windows.h>
#include <intrin.h>

#include <atomic>

namespace clk 
{
	namespace 
	{
		std::atomic<bool> g_tsc{false};
		std::atomic<double> g_perSecond{1e7};		// Until Init: system time, in 100 ns units
		
		Ticks SystemTime() 
		{
			FILETIME ft;
			GetSystemTimePreciseAsFileTime(&ft);
			return (static_cast<Ticks>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
		}
		
		bool InvariantTsc() 
		{
			int regs[4]{};
			__cpuid(regs, static_cast<int>(0x80000000));
			if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
			__cpuid(regs, static_cast<int>(0x80000007));
			return (regs[3] & (1 << 8)) != 0;				// EDX bit 8: invariant TSC
		}
	}
	
	void Init() 
	{
		if (!InvariableTsc()) return;
		// Calibrate against the system clock over ~20 ms (good to ~0.01%).
		const Ticks s0 = SystemTime(), t0 = __rdtsc();
		Sleep(20);
		const Ticks s1 = SystemTime(), t1 = __rdtsc();
		const double seconds = static_cast<double>(s1 - s0) * 1e-7;
		if (seconds <= 0.0 || t1 <= t0) return;
		g_perSecond.store(static_cast<double>(t1 - t0) / seconds, std::memory_order_relaxed);
		g_tsc.store(true, std::memory_order_relaxed);
	}
	
	Ticks Now() { return g_tsc.load(std::memory_order_acquire) ? __rdtsc() : SystemTime(); }
	
	double Seconds(Ticks ticks) { return static_cast<double>(ticks) / g_perSecond.load(std::memory_order_relaxed); }
	
	Ticks FromSeconds(double seconds) 
	{
		return seconds <= 0.0 ? 0 : static_cast<Ticks>(seconds * g_perSecond.load(std::memory_order_relaxed));
	}
}