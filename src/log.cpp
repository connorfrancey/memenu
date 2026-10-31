#include "log.h"
#include "paths.h"

#include <spdlog/spdlog.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace {
    std::filesystem::path g_file;

    std::mutex g_mutex;                 // Guards g_entries/g_nextId (read from the render thread)
    std::deque<logging::Entry> g_entries;
    std::uint64_t g_nextId = 1;

    // Keeps the last kMaxEntries lines in memory. Locking is done here, so the sink itself needs no mutex.
    class MemorySink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
    protected:
        void sink_it_(const spdlog::details::log_msg& msg) override
        {
            using namespace std::chrono;
            const auto t = system_clock::to_time_t(msg.time);
            const auto ms = duration_cast<milliseconds>(msg.time.time_since_epoch()).count() % 1000;
            std::tm tm{};
            localtime_s(&tm, &t);
            char time[16];
            std::snprintf(time, sizeof time, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));

            const auto level = static_cast<logging::Level>(std::min<int>(msg.level, spdlog::level::critical));
            const std::string_view payload(msg.payload.data(), msg.payload.size());

            std::lock_guard lock(g_mutex);
            std::size_t start = 0;
            do {
                std::size_t end = payload.find('\n', start);
                if (end == std::string_view::npos) end = payload.size();
                std::string_view line = payload.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                g_entries.push_back({g_nextId++, time, level, std::string(line)});
                start = end + 1;
            } while (start < payload.size());

            while (g_entries.size() > logging::kMaxEntries) g_entries.pop_front();
        }
        void flush_() override {}
    };
}

void logging::Init() {
    try {
        // TODO: Make log filename a global constant
        g_file = paths::Dir() / "memenu.log";
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(g_file.string(), /*truncate*/ true);
        auto dbg  = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto mem  = std::make_shared<MemorySink>();

        auto logger = std::make_shared<spdlog::logger>("me", spdlog::sinks_init_list{ file, dbg, mem });
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
#ifndef NDEBUG
        spdlog::set_level(spdlog::level::debug);
#endif
    } catch (const spdlog::spdlog_ex&) {
        // Logging is best-effort; never take the game down over it.
    }
}

void logging::Shutdown()
{
    if (auto logger = spdlog::default_logger()) logger->flush();
    spdlog::shutdown();
}

std::uint64_t logging::CopySince(std::uint64_t afterId, std::vector<Entry>& out)
{
    std::lock_guard lock(g_mutex);
    // Ids increase along the deque: skip to the first one that is newer.
    auto it = std::upper_bound(g_entries.begin(), g_entries.end(), afterId,
                               [](std::uint64_t id, const Entry& e) { return id < e.id; });
    out.insert(out.end(), it, g_entries.end());
    return g_entries.empty() ? afterId : std::max(afterId, g_entries.back().id);
}

const std::filesystem::path& logging::FilePath() { return g_file; }
