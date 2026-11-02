// memenu_bench: runs every benchmark group, or the ones named on the command line.
//
//     memenu_bench                    all groups
//     memenu_bench Replay Mesh        only these (case-insensitive substring of the group name)
//     memenu_bench --list             the groups
//     memenu_bench --csv         results as CSV (stdout), instead of nanobench's table
#define ANKERL_NANOBENCH_IMPLEMENT
#include "bench.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

bench::Group*& bench::Groups()
{
    static Group* head = nullptr;
    return head;
}

namespace {

bool Matches(const char* name, const std::vector<std::string>& filters)
{
    if (filters.empty()) return true;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (std::string f : filters) {
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.find(f) != std::string::npos) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> filters;
    bool list = false, csv = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--csv")) csv = true;
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            std::puts("usage: memenu_bench [--list] [--csv] [group...]");
            return 0;
        } else filters.emplace_back(argv[i]);
    }

    // Registration order is reversed by the list: collect, then sort by name for a stable output
    std::vector<bench::Group*> groups;
    for (bench::Group* g = bench::Groups(); g; g = g->next) groups.push_back(g);
    std::sort(groups.begin(), groups.end(), [](auto* a, auto* b) { return std::strcmp(a->name, b->name) < 0; });

    if (list) {
        for (auto* g : groups) std::puts(g->name);
        return 0;
    }

    int ran = 0;
    for (auto* g : groups) {
        if (!Matches(g->name, filters)) continue;
        ankerl::nanobench::Bench b;
        b.title(g->name).warmup(3).minEpochIterations(5).relative(false);
        if (csv) b.output(nullptr);
        g->run(b);
        if (csv) b.render(ankerl::nanobench::templates::csv(), std::cout);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "no benchmark group matches; --list shows them\n");
        return 1;
    }
    return 0;
}
