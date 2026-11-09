#include "runtime/packages.h"

#include "core/clock.h"
#include "runtime/memory.h"

#include <UnrealLib/Core/Log.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <Windows.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace
{
	// What the menu's code has allocated (runtime/memory.h): the game's own allocations don't blur it
	std::int64_t PrivateBytes() { return mirage::memory::LiveBytes(); }

	// UnrealLib's own messages go into the menu's log, marked as the library's (once: the sink is global)
	void RouteLibraryLog()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			UnrealLib::set_log_level(UnrealLib::LogLevel::Warning);
			UnrealLib::set_log_sink([](UnrealLib::LogLevel level, std::string_view message) {
				constexpr spdlog::level::level_enum levels[] = {spdlog::level::trace, spdlog::level::debug,
					spdlog::level::info, spdlog::level::warn, spdlog::level::err, spdlog::level::off};
				spdlog::log(levels[static_cast<int>(level)], "[unreallib] {}", message);
			});
		});
	}

	double MB(std::int64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
}

fs::path mirage::packages::CookedDir()
{
	wchar_t exe[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	return fs::path(exe).parent_path().parent_path() / "TdGame" / "CookedPC";
}

mirage::packages::Report mirage::packages::Probe(const fs::path& cooked)
{
	using namespace UnrealLib;
	RouteLibraryLog();

	Report r;
	r.cooked = cooked.empty() ? CookedDir() : cooked;
	const std::int64_t before = PrivateBytes();
	const clk::Ticks start = clk::Now();
	try {
		Linker linker;
		linker.add_search_path(r.cooked, false);      // The script packages are at the top of CookedPC
		ReflectionSchema schema(linker);

		for (const char* name : {"Core", "Engine", "TdGame"}) {
			const Package* p = linker.find_package(name);
			if (!p) throw std::runtime_error(std::string(name) + ".u was not found in " + r.cooked.string());
			schema.add_package(*p);

			PackageInfo info;
			info.name = name;
			info.exports = p->exports().size();
			for (std::size_t i = 0; i < info.exports; ++i) {
				const ObjectIndex index = ObjectIndex::from_export(i);
				const std::string cls = p->class_name(index);
				if (cls == "Function") ++info.functions;
				else if (cls == "ScriptStruct") ++info.structs;
				else if (cls == "Enum") ++info.enums;
				if (cls != "Class") continue;
				++info.classes;

				// The class, then every field it declares (children list)
				const FieldObject* f = schema.field(ObjectRef{p, index});
				if (!f || !f->structure) {
					++r.unreadable;
					continue;
				}
				++r.classesRead;
				int guard = 0;
				for (ObjectIndex c = f->structure->children; !c.is_null() && guard < 100000; ++guard) {
					const FieldObject* child = schema.field(ObjectRef{p, c});
					if (!child) {
						++r.unreadable;
						break;
					}
					if (child->kind == FieldKind::Property) ++r.properties;
					c = child->field.next;
				}
			}
			r.packages.push_back(std::move(info));
		}

		// Known answers: a struct property inherited from Engine, an enum property of the game's own pawn
		const auto location = schema.find_property("TdPawn", "Location");
		const auto move = schema.find_property("TdPawn", "MovementState");
		if (!location || location->type != PropertyType::Struct || location->struct_name != "Vector")
			throw std::runtime_error("TdPawn.Location is not read as a Vector struct property");
		if (!move || move->type != PropertyType::Byte || !move->is_enum)
			throw std::runtime_error("TdPawn.MovementState is not read as an enum byte property");

		r.peakBytes = PrivateBytes() - before;
		r.ok = r.unreadable == 0;
		if (!r.ok) r.error = std::to_string(r.unreadable) + " fields could not be read";
	} catch (const std::exception& e) {
		r.error = e.what();
	}
	r.seconds = clk::SecondsSince(start);
	r.retainedBytes = PrivateBytes() - before;

	std::string summary;
	for (const PackageInfo& p : r.packages)
		summary += fmt::format("{}{} ({} exports, {} classes, {} functions, {} structs, {} enums)",
							   summary.empty() ? "" : ", ", p.name, p.exports, p.classes, p.functions, p.structs, p.enums);
	if (r.ok)
		spdlog::info("Mirage: UnrealLib read {} in {:.2f} s: {} classes with {} properties; memory +{:.1f} MB while "
					 "loaded, +{:.1f} MB after", summary, r.seconds, r.classesRead, r.properties, MB(r.peakBytes),
					 MB(r.retainedBytes));
	else
		spdlog::error("Mirage: UnrealLib could not read the script packages in {}: {} ({:.2f} s)", r.cooked.string(),
					  r.error, r.seconds);
	return r;
}
