#pragma once
#include <cstddef>

// Tracy profiler zones (CMake option MEMENU_TRACY, `memenu build --tracy`). Without it every macro compiles to nothing.
//
//   PROF_ZONE("name")             times the rest of the enclosing scope, under a fixed name
//   PROF_ZONE_NAMED(str, len)     the same, named at run time (a module's name); the text is copied
//   PROF_FRAME()                  end of a presented frame (the menu's EndScene on the back buffer)
//   PROF_FRAME_NAMED("name")      end of another kind of frame (an engine tick); the name must be a string literal
//   PROF_PLOT("name", value)      a value over time (scheduler work, ...)
//   PROF_THREAD("name")           names the calling thread in the profiler (once per thread is enough)
//
// Usage: run the game with a --tracy build, open the Tracy profiler
// (the same version as TRACY_VERSION in cmake/Versions.cmake) and connect to localhost. Nothing is collected until then.
#ifndef MEMENU_TRACY
#define MEMENU_TRACY 0
#endif

namespace prof
{
	inline constexpr bool kEnabled = MEMENU_TRACY != 0;

	// Starts the profiler's threads (menu thread, not DllMain: the loader lock) and stops them before the DLL unloads.
	// Zones only record between the two; outside them they do nothing. No-ops in a build without Tracy.
	void Start();
	void Stop();
	bool Running();
}

#if MEMENU_TRACY
#include <tracy/Tracy.hpp>

#define MEMENU_PROF_CAT2(a, b) a##b
#define MEMENU_PROF_CAT(a, b) MEMENU_PROF_CAT2(a, b)
#define MEMENU_PROF_VAR MEMENU_PROF_CAT(memenu_prof_zone_, __LINE__)	// Nested zones don't shadow each other

#define PROF_ZONE(name) ZoneNamedN(MEMENU_PROF_VAR, name, ::prof::Running())
#define PROF_ZONE_NAMED(str, len)                                                                                        \
	ZoneNamed(MEMENU_PROF_VAR, ::prof::Running());                                                                       \
	if (MEMENU_PROF_VAR.IsActive()) MEMENU_PROF_VAR.Name(str, len)
#define PROF_FRAME()                                                                                                     \
	do { if (::prof::Running()) FrameMark; } while (0)
#define PROF_FRAME_NAMED(name)                                                                                           \
	do { if (::prof::Running()) FrameMarkNamed(name); } while (0)
#define PROF_PLOT(name, value)                                                                                           \
	do { if (::prof::Running()) TracyPlot(name, value); } while (0)
#define PROF_THREAD(name)                                                                                                \
	do { static thread_local bool memenu_prof_named = false;                                                             \
	     if (!memenu_prof_named && ::prof::Running()) { memenu_prof_named = true; tracy::SetThreadName(name); } } while (0)
#else
#define PROF_ZONE(name) ((void)0)
#define PROF_ZONE_NAMED(str, len) ((void)0)
#define PROF_FRAME() ((void)0)
#define PROF_FRAME_NAMED(name) ((void)0)
#define PROF_PLOT(name, value) ((void)0)
#define PROF_THREAD(name) ((void)0)
#endif
