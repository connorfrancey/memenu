// UnrealLib status check (`memenu unreallib`): runs one check per UnrealLib module against the game's own packages
// and logs what each one found, so it is easy to see that the library really works here (and how fast it is).
//
//   unreallib_status [CookedPC folder]
//
// Without an argument the game is found like the launcher finds it (or through UNREALLIB_MIRRORSEDGE_DIR).
// Exit code: 0 when every check passed, 1 when one failed, 2 when the game wasn't found.

#include "detect.h"

#include <UnrealLib/UnrealLib.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace UnrealLib;

namespace
{
    fs::path FindCookedPC(int argc, char** argv)
    {
        if (argc > 1) return argv[1];
        if (const char* dir = std::getenv("UNREALLIB_MIRRORSEDGE_DIR"); dir && *dir) return dir;
        for (const auto& install : detect::FindInstalls())
        {
            // ...\Binaries\MirrorsEdge.exe -> ...\TdGame\CookedPC
            const fs::path cooked = install.exe.parent_path().parent_path() / "TdGame" / "CookedPC";
            if (fs::exists(cooked / "Core.u")) return cooked;
        }
        return {};
    }

    // UnrealLib's own messages (warnings about the packages, ...) go into the same log, marked as the library's
    void RouteLibraryLog()
    {
        set_log_level(LogLevel::Info);
        set_log_sink([](LogLevel level, std::string_view message) {
            constexpr spdlog::level::level_enum levels[] = {spdlog::level::trace, spdlog::level::debug,
                spdlog::level::info, spdlog::level::warn, spdlog::level::err, spdlog::level::off};
            spdlog::log(levels[static_cast<int>(level)], "  [unreallib] {}", message);
        });
    }

    struct Checks
    {
        int passed = 0;
        int failed = 0;

        // Runs one check: it logs its own findings and returns false (or throws) when it failed
        void Run(const char* name, const std::function<bool()>& check)
        {
            spdlog::info("{} ...", name);
            const auto start = std::chrono::steady_clock::now();
            bool ok = false;
            try
            {
                ok = check();
            }
            catch (const Error& e)
            {
                spdlog::error("  UnrealLib error: {}", e.what());
            }
            catch (const std::exception& e)
            {
                spdlog::error("  exception: {}", e.what());
            }
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ok)
            {
                ++passed;
                spdlog::info("  OK   {} ({:.1f} ms)", name, ms);
            }
            else
            {
                ++failed;
                spdlog::error("  FAIL {} ({:.1f} ms)", name, ms);
            }
        }
    };

    void LogDiagnostics(const Diagnostics& diagnostics, std::size_t shown = 3)
    {
        for (const Diagnostic& d : diagnostics.entries())
        {
            if (d.severity == Severity::Info) continue;
            if (shown-- == 0) break;
            spdlog::warn("    {} {}: {}", to_string(d.severity), d.code, d.message);
        }
    }

    // The first map in the install, in name order (the same one every run)
    fs::path FirstMap(const fs::path& cooked)
    {
        std::vector<fs::path> maps;
        for (const auto& entry : fs::recursive_directory_iterator(cooked / "Maps"))
            if (entry.is_regular_file() && entry.path().extension() == ".me1") maps.push_back(entry.path());
        if (maps.empty()) return {};
        return *std::min_element(maps.begin(), maps.end());
    }
}

int main(int argc, char** argv)
{
    spdlog::set_default_logger(spdlog::stdout_color_mt("unreallib"));
    spdlog::set_pattern("%H:%M:%S.%e %^%-5l%$ %v");
    RouteLibraryLog();

    spdlog::info("UnrealLib {}", library_version_string);
    const fs::path cooked = FindCookedPC(argc, argv);
    if (cooked.empty() || !fs::exists(cooked / "Core.u"))
    {
        spdlog::error("Mirror's Edge not found: pass its TdGame/CookedPC folder, or set UNREALLIB_MIRRORSEDGE_DIR");
        return 2;
    }
    spdlog::info("Game packages: {}", cooked.string());

    Checks checks;
    Linker linker;
    ReflectionSchema schema(linker);
    std::vector<const Package*> scripts;

    checks.Run("Package: read Core.u", [&] {
        Package package;
        package.load(cooked / "Core.u");
        const auto& summary = package.summary();
        spdlog::info("  file version {}, licensee {}; {} names, {} imports, {} exports", summary.file_version,
                     summary.licensee_version, package.names().size(), package.imports().size(),
                     package.exports().size());
        const bool mirrors_edge = summary.file_version == 536 && summary.licensee_version == 43;
        if (!mirrors_edge) spdlog::error("  expected Mirror's Edge PC (536/43)");
        return mirrors_edge && package.find_export("Object").has_value();
    });

    checks.Run("Linker: index the install", [&] {
        linker.add_search_path(cooked);
        spdlog::info("  {} package files found", linker.indexed_count());
        return linker.indexed_count() > 0;
    });

    checks.Run("Validator: Core, Engine, TdGame", [&] {
        bool ok = true;
        for (const char* name : {"Core", "Engine", "GameFramework", "TdGame"})
        {
            const Package* package = linker.find_package(name);
            if (!package)
            {
                spdlog::error("  {} didn't load", name);
                ok = false;
                continue;
            }
            scripts.push_back(package);
            const Diagnostics diagnostics = validate_package(*package);
            spdlog::info("  {}: {} exports, {} errors, {} warnings", name, package->exports().size(),
                         diagnostics.count(Severity::Error), diagnostics.count(Severity::Warning));
            LogDiagnostics(diagnostics);
            ok = ok && !diagnostics.has_errors();
        }
        return ok;
    });

    checks.Run("Linker: resolve TdGame's imports", [&] {
        const Package* td = linker.find_package("TdGame");
        if (!td) return false;
        // A package the game doesn't ship (TdGame imports from CustomStretches, which isn't in the install) is the
        // game's data, not a library failure: only the other reasons fail the check
        std::size_t resolved = 0, missing_package = 0, broken = 0;
        std::map<std::string, int> unresolved;   // "<reason> <package>" -> imports
        for (std::size_t i = 0; i < td->imports().size(); ++i)
        {
            const ObjectIndex index = ObjectIndex::from_import(i);
            const Resolution& r = linker.resolve(*td, index);
            if (r.ok())
            {
                ++resolved;
                continue;
            }
            const std::string path = td->object_path(index);
            ++unresolved[fmt::format("{} {}", to_string(r.status), path.substr(0, path.find('.')))];
            if (r.status == ResolveStatus::PackageNotFound) ++missing_package;
            else if (broken++ < 3) spdlog::error("    {}: {} {}", path, to_string(r.status), r.detail);
        }
        spdlog::info("  {} of {} imports resolved", resolved, td->imports().size());
        for (const auto& [what, count] : unresolved) spdlog::warn("    {:4} unresolved: {}", count, what);
        if (missing_package) spdlog::info("  {} imports name packages the game doesn't ship (not an error)", missing_package);
        return broken == 0;
    });

    checks.Run("Linker: class hierarchy (TdPawn is a Pawn)", [&] {
        const Package* td = linker.find_package("TdGame");
        const auto pawn = td ? td->find_export("TdPawn") : std::nullopt;
        if (!pawn) return false;
        const auto is_pawn = linker.class_is_a({td, *pawn}, "Engine.Pawn");
        spdlog::info("  TdPawn derives from Engine.Pawn: {}", is_pawn ? (*is_pawn ? "yes" : "no") : "unknown");
        return is_pawn.value_or(false);
    });

    checks.Run("Properties: TdPawn's defaults", [&] {
        for (const Package* p : scripts) schema.add_package(*p);
        const Package* td = linker.find_package("TdGame");
        const auto defaults = td ? td->find_export("Default__TdPawn") : std::nullopt;
        if (!defaults) return false;
        const ObjectProperties properties = linker.read_properties({td, *defaults}, &schema);
        PropertyFormatOptions format;
        format.names = &td->names();
        format.object_name = [&](ObjectIndex i) { return td->object_path(i); };
        spdlog::info("  {} properties, for example:", properties.properties.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(3, properties.properties.size()); ++i)
        {
            const Property& p = properties.properties[i];
            spdlog::info("    {} = {}", p.name, format_property_value(p.value, format));
        }
        return !properties.properties.empty();
    });

    checks.Run("Decompiler: TdPawn.Tick", [&] {
        const Package* td = linker.find_package("TdGame");
        const auto tick = td ? td->find_export("TdPawn.Tick") : std::nullopt;
        if (!tick) return false;
        Decompiler::ScriptDecompiler decompiler(linker, Decompiler::NativeTable::from_packages(linker, scripts));
        decompiler.set_schema(&schema);
        const Decompiler::DecompiledScript script = decompiler.decompile({td, *tick});
        const auto lines = std::count(script.source.begin(), script.source.end(), '\n');
        spdlog::info("  {} lines, {} unknown natives, {} placeholders", lines, script.unknown_natives,
                     script.placeholders);
        const auto first_line = script.source.substr(0, script.source.find('\n'));
        spdlog::info("    {}", first_line);
        return lines > 0;
    });

    checks.Run("Map: read the first level", [&] {
        const fs::path file = FirstMap(cooked);
        if (file.empty())
        {
            spdlog::error("  no .me1 maps under {}", (cooked / "Maps").string());
            return false;
        }
        const Package& map = linker.load(file);
        std::map<std::string, int> classes;
        for (std::size_t i = 0; i < map.exports().size(); ++i)
            ++classes[map.class_name(ObjectIndex::from_export(i))];
        std::vector<std::pair<std::string, int>> top(classes.begin(), classes.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        spdlog::info("  {}: {} exports of {} classes; most common:", map.name(), map.exports().size(), classes.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(5, top.size()); ++i)
            spdlog::info("    {:5} {}", top[i].second, top[i].first);
        return !map.exports().empty();
    });

    const int total = checks.passed + checks.failed;
    if (checks.failed == 0) spdlog::info("UnrealLib works: {}/{} checks passed", checks.passed, total);
    else spdlog::error("UnrealLib: {}/{} checks failed", checks.failed, total);
    return checks.failed == 0 ? 0 : 1;
}
