#pragma once

// Optional performance optimizations (CMake option MEMENU_OPTIMIZATIONS, `memenu build --no-optimizations`).
//
// With them off, everything still works, it just does the work every time:
//   cache::Value          recomputes on every Get() (statistics still count, as misses)
//   game::ObjectRef       scans the object list on every Find(): no remembered pointer, no retry back-off
//   game::NameIndex       walks the engine's name table on every call
//   sched::Tick           runs every enabled entry on every tick: no intervals, no NeedsUpdate() skipping,
//                         no budget deferral or adaptive interval stretching
// Run order (`after`, priority) and `requiredOn` are not optimizations: they behave the same either way.
//
// Nothing may depend on an optimization for correctness: a feature that only works because a value is cached or a
// module runs less often is a bug, and a build without them is how to find it.
#ifndef MEMENU_OPTIMIZATIONS
#define MEMENU_OPTIMIZATIONS 1
#endif

namespace opt
{
	inline constexpr bool kEnabled = MEMENU_OPTIMIZATIONS != 0;
}
