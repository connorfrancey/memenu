#include "core/profile.h"

#include <atomic>

#if MEMENU_TRACY
#include <spdlog/spdlog.h>
#endif

namespace
{
	std::atomic<bool> g_running{false};
}

namespace prof
{
	void Start()
	{
#if MEMENU_TRACY
		if (g_running.exchange(true)) return;
		tracy::StartupProfiler();
		TracySetProgramName("Mirror's Edge (medge_menu)");
		spdlog::info("Tracy profiler client running: connect the Tracy profiler (same version) to localhost");
#endif
	}

	void Stop()
	{
#if MEMENU_TRACY
		if (!g_running.exchange(false)) return;
		tracy::ShutdownProfiler();
#endif
	}

	bool Running() { return g_running.load(std::memory_order_relaxed); }
}
