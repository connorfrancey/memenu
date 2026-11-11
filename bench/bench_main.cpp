// memenu_bench: runs every benchmark, or the groups named on the command line (Google Benchmark).
//
//     memenu_bench                    all of them
//     memenu_bench Replay Mesh        only these groups (case-insensitive substring of the group name)
//     memenu_bench --list             the benchmarks
//     memenu_bench --csv              results as CSV (stdout), instead of the table
//     memenu_bench --benchmark_...    any of Google Benchmark's own flags (--benchmark_repetitions=5, ...)
#include "bench.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// A group filter as a case-insensitive regex part: letters become [aA], everything else is escaped
std::string Pattern(const std::string& text)
{
    std::string out;
    for (unsigned char c : text) {
        if (std::isalpha(c)) {
            out += '[';
            out += static_cast<char>(std::tolower(c));
            out += static_cast<char>(std::toupper(c));
            out += ']';
        } else if (std::isalnum(c) || c == ' ' || c == '_') {
            out += static_cast<char>(c);
        } else {
            out += '\\';
            out += static_cast<char>(c);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args{argv[0]}, groups;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) args.emplace_back("--benchmark_list_tests=true");
        else if (!std::strcmp(argv[i], "--csv")) args.emplace_back("--benchmark_format=csv");
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            std::puts("usage: memenu_bench [--list] [--csv] [--benchmark_...] [group...]");
            return 0;
        } else if (!std::strncmp(argv[i], "--", 2)) args.emplace_back(argv[i]);
        else groups.emplace_back(argv[i]);
    }
    if (!groups.empty()) {
        std::string filter = "--benchmark_filter=^[^/]*(";
        for (std::size_t i = 0; i < groups.size(); ++i) filter += (i ? "|" : "") + Pattern(groups[i]);
        args.push_back(filter + ")[^/]*/");
    }

    std::vector<char*> raw;
    for (auto& a : args) raw.push_back(a.data());
    int n = static_cast<int>(raw.size());
    benchmark::Initialize(&n, raw.data());
    if (benchmark::ReportUnrecognizedArguments(n, raw.data())) return 1;
    const std::size_t ran = benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    if (ran == 0) {
        std::fprintf(stderr, "no benchmark matches; --list shows them\n");
        return 1;
    }
    return 0;
}
