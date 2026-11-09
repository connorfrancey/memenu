#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The game's script packages read with UnrealLib inside the game (Mirage, step 1). For now a probe: it proves the
// library works in this process (32-bit, static CRT, next to the engine) and measures what it costs. Step 2 builds the
// live engine model on it.
namespace mirage::packages
{
	struct PackageInfo
	{
		std::string name;
		std::size_t exports = 0, classes = 0, functions = 0, structs = 0, enums = 0;
	};

	struct Report
	{
		bool ok = false;					// Everything read, and the known answers matched
		std::string error;					// Why not
		std::filesystem::path cooked;		// The CookedPC folder read from
		std::vector<PackageInfo> packages;	// Core, Engine, TdGame
		std::size_t classesRead = 0;		// Classes whose reflection data was read, with all their fields
		std::size_t properties = 0;			// Properties declared by those classes
		std::size_t unreadable = 0;			// Fields UnrealLib could not read
		double seconds = 0.0;
		std::int64_t peakBytes = 0;			// Memory the menu allocated for the loaded packages (runtime/memory.h)
		std::int64_t retainedBytes = 0;		// ... and still allocated after everything was released
	};

	// <the game's Binaries>\..\TdGame\CookedPC
	std::filesystem::path CookedDir();

	// Loads Core, Engine and TdGame with UnrealLib and reads every class's reflection data (fields, property types),
	// checks a few known answers, measures the time and memory, then releases everything. Takes seconds and blocks:
	// never on the game or render thread. Logged.
	Report Probe(const std::filesystem::path& cooked = {});
}
