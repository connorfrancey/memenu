// mirage_sdkgen: writes the Mirage SDK from the game's script packages (Mirage, step 3).
//
//   mirage_sdkgen [--config sdkgen.toml] [--cooked <CookedPC>] [--out <sdk>] [--reference <dir> | --no-reference]
//                 [--old-sdk <dir>] [--report <file>]
//
// Reads sdkgen.toml (default: next to the executable's source, tools/sdkgen/sdkgen.toml, found from the current
// folder), then applies the options. Exit code 0 when everything was written, 1 on an error, 2 on bad arguments.

#include "config.h"
#include "emit.h"
#include "model.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	void Usage()
	{
		std::puts("mirage_sdkgen: writes the Mirage SDK (C++ headers, Lua stubs, decompiled script) from the game's packages\n\n"
				  "  --config <file>      sdkgen.toml to read (default: tools/sdkgen/sdkgen.toml)\n"
				  "  --cooked <dir>       the game's TdGame\\CookedPC\n"
				  "  --engine <file>      engine-only layouts (engine-layouts.json)\n"
				  "  --out <dir>          where the C++ SDK goes\n"
				  "  --reference <dir>    where the decompiled script and Lua stubs go\n"
				  "  --no-reference       don't write them\n"
				  "  --old-sdk <dir>      the old (mmultiplayer) SDK, for the report of what it has wrong\n"
				  "  --report <file>      where that report goes\n");
	}

	std::filesystem::path DefaultConfig()
	{
		for (std::filesystem::path dir = std::filesystem::current_path(); !dir.empty(); dir = dir.parent_path()) {
			const auto candidate = dir / "tools" / "sdkgen" / "sdkgen.toml";
			if (std::filesystem::exists(candidate)) return candidate;
			if (dir == dir.parent_path()) break;
		}
		return "sdkgen.toml";
	}
}

int main(int argc, char** argv)
{
	using Clock = std::chrono::steady_clock;
	const auto start = Clock::now();

	std::filesystem::path config;
	for (int i = 1; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], "--config")) config = argv[i + 1];
	if (config.empty()) config = DefaultConfig();

	sdkgen::Options o;
	std::string error;
	if (!sdkgen::LoadConfig(config, o, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
		const auto set = [&](std::filesystem::path& to) {
			const char* v = value();
			if (!v) return false;
			to = std::filesystem::absolute(v);
			return true;
		};
		bool ok = true;
		if (a == "--help" || a == "-h") {
			Usage();
			return 0;
		} else if (a == "--config") {
			ok = value() != nullptr;
		} else if (a == "--cooked") {
			ok = set(o.cooked);
		} else if (a == "--engine") {
			ok = set(o.engine);
		} else if (a == "--out") {
			ok = set(o.out);
		} else if (a == "--reference") {
			ok = set(o.reference);
		} else if (a == "--no-reference") {
			o.reference.clear();
		} else if (a == "--old-sdk") {
			ok = set(o.oldSdk);
		} else if (a == "--report") {
			ok = set(o.report);
		} else {
			ok = false;
		}
		if (!ok) {
			std::fprintf(stderr, "error: bad argument %s\n\n", a.c_str());
			Usage();
			return 2;
		}
	}
	if (o.cooked.empty()) {
		std::fprintf(stderr, "error: the game's CookedPC folder was not found (set [game] cooked in %s, or --cooked)\n",
					 config.string().c_str());
		return 1;
	}
	if (o.out.empty() || o.statics.empty() || o.engine.empty()) {
		std::fprintf(stderr, "error: %s needs [output] sdk, [input] static and [input] engine_layouts\n",
					 config.string().c_str());
		return 1;
	}

	std::printf("config    %s\ncooked    %s\n", config.string().c_str(), o.cooked.string().c_str());
	sdkgen::Model model;
	if (!sdkgen::Build(model, o.cooked, o.engine, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}
	for (const auto& w : model.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());

	sdkgen::Stats stats;
	if (!sdkgen::EmitCpp(model, o, stats, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}
	std::printf("sdk       %s: %zu classes, %zu structs, %zu functions (%zu not generated), %zu warnings\n",
				o.out.string().c_str(), stats.classes, stats.structs, stats.functions, stats.skippedFunctions,
				stats.warnings.size());

	if (!o.reference.empty()) {
		if (!sdkgen::EmitLua(model, o, error) || !sdkgen::EmitScript(model, o, error)) {
			std::fprintf(stderr, "error: %s\n", error.c_str());
			return 1;
		}
		std::printf("reference %s\n", o.reference.string().c_str());
	}
	if (!o.oldSdk.empty()) {
		if (!sdkgen::CompareOldSdk(model, o, error)) {
			std::fprintf(stderr, "error: %s\n", error.c_str());
			return 1;
		}
		std::printf("report    %s\n", o.report.string().c_str());
	}
	std::printf("done in %.1f s\n", std::chrono::duration<double>(Clock::now() - start).count());
	return 0;
}
