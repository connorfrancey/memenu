#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace logging {
    void Init();        // Default spdlog logger -> memenu.log + OutputDebugString + in-memory buffer
    void Shutdown();

    // The last lines that were logged, kept in memory for the log viewer (features/debug/log_viewer.cpp).
    enum class Level { Trace, Debug, Info, Warn, Error, Critical };
    struct Entry {
        std::uint64_t id;           // Increasing, never reused (TODO: Maybe replace with uuid system).
        std::string time;           // "HH:MM:SS.mmm"
        Level level;
        std::string text;           // One-line; multi-line messages become several entries
    };
    constexpr std::size_t kMaxEntries = 4000;

    // Appends the entries newer than `afterId` to `out` (any thread) and returns the id of the newest one.
    std::uint64_t CopySince(std::uint64_t afterId, std::vector<Entry>& out);
    const std::filesystem::path& FilePath();
}
