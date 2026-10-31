#include "log.h"
#include "build_info.h"
#include "paths.h"

#include <spdlog/spdlog.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>
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

// ---------------------------------------------------------------------------------------------------------------------
// Log files
//
//   <DLL folder>/logs/memenu.log                 the current session
//   <DLL folder>/logs/.old/<name>.log.zst        earlier sessions, zstd-compressed at the next start (newest kKeepOld)
//       <name> = memenu_<started yyyymmdd-hhmmss>_b<build number>_<magic>, e.g. memenu_20261031-181040_b42_9F3A12C7E4B0D851
//
// Format (v1): a header of "# key  value" lines, a blank line, then one line per message:
//
//   # memenu-log 1
//   # started   2026-10-31T18:10:40+0100          local time the session started, with its UTC offset
//   # build     42                                 build number (build_info::kNumber)
//   # magic     9F3A12C7E4B0D851                   hash of the "detail" lines below (build_info::kMagic)
//   # id        0c8e...                            build UUID
//   # built     2026-10-31T17:10:32Z
//   # detail    version=0.1.0                      the build's details, one per line; the magic is the SHA-256
//   # detail    commit=b8705ae0a5-dirty            (first 16 hex digits) of them joined with newlines
//   # detail    ...
//
//   [2026-10-31 18:10:40.123 +01:00] [info] message
//
// The header is plain text so the log stays readable as-is; `memenu --copy-info` and the archive names read it.
namespace {
    namespace fs = std::filesystem;
    constexpr const char* kLogName = "memenu.log";
    constexpr std::size_t kKeepOld = 100;   // Archived sessions kept in logs/.old
    constexpr int kZstdLevel = 9;           // Logs are small: a good ratio at little cost

    std::string HeaderValue(const fs::path& log, const std::string& key)
    {
        std::ifstream in(log);
        std::string line;
        for (int i = 0; i < 32 && std::getline(in, line); ++i) {
            if (line.rfind("# ", 0) != 0) break;
            std::istringstream ss(line.substr(2));
            std::string k, v;
            ss >> k >> v;
            if (k == key) return v;
        }
        return {};
    }

    // The archive name for a finished log: from its header, or from the file time for logs older than this format.
    std::string ArchiveName(const fs::path& log)
    {
        std::string started = HeaderValue(log, "started");
        std::string stamp;
        for (char c : started.substr(0, 19)) if (std::isdigit(static_cast<unsigned char>(c))) stamp += c;
        if (stamp.size() == 14) {
            stamp.insert(8, "-");
        } else {   // No header: when it was last written
            std::error_code ec;
            const auto ft = fs::last_write_time(log, ec);
            const auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            std::tm tm{};
            localtime_s(&tm, &t);
            char buf[32];
            std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
            stamp = buf;
        }
        std::string build = HeaderValue(log, "build"), magic = HeaderValue(log, "magic");
        return "memenu_" + stamp + "_b" + (build.empty() ? "0" : build) + "_" + (magic.empty() ? "unknown" : magic);
    }

    bool Compress(const fs::path& src, const fs::path& dst, std::string& error)
    {
        std::ifstream in(src, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && !in.eof()) { error = "cannot read it"; return false; }

        std::vector<char> out(ZSTD_compressBound(data.size()));
        const std::size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), kZstdLevel);
        if (ZSTD_isError(size)) { error = ZSTD_getErrorName(size); return false; }

        const fs::path tmp = dst.string() + ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            o.write(out.data(), static_cast<std::streamsize>(size));
            if (!o) { error = "cannot write " + tmp.string(); return false; }
        }
        std::error_code ec;
        fs::rename(tmp, dst, ec);
        if (ec) { fs::remove(tmp, ec); error = "cannot rename to " + dst.string(); return false; }
        return true;
    }

    // Moves the previous session's log (and a memenu.log left next to the DLL by older builds) into logs/.old.
    // Returns what happened, logged once the logger exists.
    std::vector<std::string> ArchivePrevious(const fs::path& logs)
    {
        std::vector<std::string> notes;
        const fs::path old = logs / ".old";
        std::error_code ec;
        fs::create_directories(old, ec);
        if (ec) { notes.push_back("cannot create " + old.string() + ": " + ec.message()); return notes; }

        for (const fs::path& prev : {logs / kLogName, paths::Dir() / kLogName}) {
            if (!fs::exists(prev, ec)) continue;
            if (fs::file_size(prev, ec) == 0 || ec) { fs::remove(prev, ec); continue; }

            const std::string name = ArchiveName(prev);
            fs::path dst = old / (name + ".log.zst");
            for (int n = 2; fs::exists(dst, ec); ++n) dst = old / (name + "-" + std::to_string(n) + ".log.zst");

            std::string error;
            const auto before = fs::file_size(prev, ec);
            if (Compress(prev, dst, error)) {
                fs::remove(prev, ec);
                notes.push_back("Previous log archived: " + dst.filename().string() + " (" + std::to_string(before / 1024) +
                                " KB -> " + std::to_string(fs::file_size(dst, ec) / 1024) + " KB)");
            } else {
                notes.push_back("Could not archive " + prev.string() + ": " + error + " (it will be overwritten)");
            }
        }

        // Keep the newest kKeepOld (names start with the date, so name order is age order)
        std::vector<fs::path> archived;
        for (const auto& e : fs::directory_iterator(old, ec))
            if (e.path().extension() == ".zst") archived.push_back(e.path());
        if (archived.size() > kKeepOld) {
            std::sort(archived.begin(), archived.end());
            for (std::size_t i = 0; i < archived.size() - kKeepOld; ++i) fs::remove(archived[i], ec);
            notes.push_back("Removed " + std::to_string(archived.size() - kKeepOld) + " oldest archived log(s)");
        }
        return notes;
    }

    void WriteHeader(const fs::path& file)
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char started[40];
        std::strftime(started, sizeof started, "%Y-%m-%dT%H:%M:%S%z", &tm);

        std::ofstream out(file, std::ios::trunc);
        out << "# memenu-log 1\n"
            << "# started   " << started << "\n"
            << "# build     " << build_info::kNumber << "\n"
            << "# magic     " << build_info::kMagicHex << "\n"
            << "# id        " << build_info::kId << "\n"
            << "# built     " << build_info::kTimestamp << "\n";
        std::istringstream details(build_info::kDetails);
        for (std::string line; std::getline(details, line);) out << "# detail    " << line << "\n";
        out << "\n";
    }
}

void logging::Init() {
    std::vector<std::string> notes;
    try {
        const fs::path logs = paths::Dir() / "logs";
        notes = ArchivePrevious(logs);
        g_file = logs / kLogName;
        WriteHeader(g_file);
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(g_file.string(), /*truncate*/ false);
        auto dbg  = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto mem  = std::make_shared<MemorySink>();

        auto logger = std::make_shared<spdlog::logger>("me", spdlog::sinks_init_list{ file, dbg, mem });
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [%^%l%$] %v");
        logger->flush_on(spdlog::level::trace);   // Every line: a crash must not lose the lines that lead up to it
        spdlog::set_default_logger(std::move(logger));
#ifndef NDEBUG
        spdlog::set_level(spdlog::level::debug);
#endif

        // Also in the in-game log viewer, which does not show the file header
        spdlog::info("memenu {} build #{} ({}, {}), magic {}", build_info::kVersion, build_info::kNumber,
                     build_info::kGitCommit, build_info::kConfig, build_info::kMagicHex);
        for (const std::string& note : notes) spdlog::info("{}", note);
    } catch (const std::exception& e) {
        // Logging is best-effort; never take the game down over it. Say why in the debugger output (DebugView).
        OutputDebugStringA((std::string("[memenu] logging disabled: cannot open ") + g_file.string() + ": " + e.what() + "\n").c_str());
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
