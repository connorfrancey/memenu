#pragma once

// Optional performance optimizations (CMake option MEMENU_OPTIMIZATIONS, `memenu build --no-optimizations`).
//
// With them off, everything still works, it just does the work every time:
//   cache::Value          recomputes on every Get() (statistics still count, as misses)
//   cache::TtlMap/Lru     never find anything: Get() computes every time
//   mem::Cache            reads the memory on every Get()
//   sig::Cache            scans on every Find(); nothing is loaded from or saved to cache/signatures.json
//   addr::Cache           resolves (or follows the pointer chain) on every call
//   cache::Memo           runs the function on every call
//   cache::NegativeCache  remembers no failure: every lookup runs
//   game::ObjectCache     asks the SDK on every Find()
//   hw::CurrentMemory     queries on every call (hw::Get's fixed hardware info is always kept)
// str::Intern is not optional: Atoms compare by identity, so equal strings must share one copy.
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
