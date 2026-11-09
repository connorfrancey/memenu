#pragma once
#include <nanobench.h>

#include <string>

// Every benchmark file registers its groups with MEMENU_BENCH; bench_main.cpp runs the ones the command line asks for.
//
//     MEMENU_BENCH(Replay) { ankerl::nanobench::Bench b; b.title("Replay"); ... ; return b; }
namespace bench {

struct Group {
    const char* name;
    void (*run)(ankerl::nanobench::Bench& bench);
    Group* next;
};

Group*& Groups();

struct Register {
    Register(Group& g)
    {
        g.next = Groups();
        Groups() = &g;
    }
};

} // namespace bench

#define MEMENU_BENCH(name)                                                         \
    static void Bench_##name(ankerl::nanobench::Bench& bench);                     \
    static bench::Group g_group_##name{#name, &Bench_##name, nullptr};             \
    static bench::Register g_register_##name{g_group_##name};                      \
    static void Bench_##name(ankerl::nanobench::Bench& bench)
