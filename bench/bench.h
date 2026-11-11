#pragma once
#include <benchmark/benchmark.h>

#include <string>
#include <utility>

// Benchmarks are Google Benchmark functions named "Group/what it measures": bench_main.cpp picks groups by the start of
// that name (memenu_bench Replay Mesh), the rest of Google Benchmark's flags (--benchmark_*) pass through.
//
//     BENCHMARK(InputHash)->Name("Replay/InputHash (10 min)");
namespace bench {

// Keeps a result alive without the deprecated const& DoNotOptimize
template <class T> void Keep(T&& value)
{
    auto copy = std::forward<T>(value);
    benchmark::DoNotOptimize(copy);
}

// Reports the time per item too ("t/frame"): what a benchmark that handles `items` things per iteration costs each
inline void PerItem(benchmark::State& state, double items, const char* unit)
{
    state.counters[std::string("t/") + unit] =
        benchmark::Counter(items, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

} // namespace bench
