#include "log.h"
#include "paths.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

void logging::Init()
{
    try {
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            (paths::Dir() / "MirrorsEdgeMenu.log").string(), /*truncate*/ true);
        auto dbg  = std::make_shared<spdlog::sinks::msvc_sink_mt>();

        auto logger = std::make_shared<spdlog::logger>("me", spdlog::sinks_init_list{ file, dbg });
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
    spdlog::shutdown();
}
