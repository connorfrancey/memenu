#pragma once
#include "model.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace sdkgen
{
	struct Options
	{
		std::filesystem::path cooked;		// The game's TdGame\CookedPC
		std::filesystem::path engine;		// engine-layouts.json
		std::filesystem::path statics;		// tools/sdkgen/static
		std::filesystem::path out;			// The SDK (sdk/)
		std::filesystem::path reference;	// Decompiled script and Lua stubs ("" = not written)
		std::filesystem::path oldSdk;		// The mmultiplayer SDK, to compare with ("" = no report)
		std::filesystem::path report;		// Where the comparison goes
	};

	struct Stats
	{
		std::size_t classes = 0, structs = 0, functions = 0, skippedFunctions = 0;
		std::vector<std::string> warnings;
	};

	bool EmitCpp(const Model& m, const Options& o, Stats& stats, std::string& error);
	// LuaLS annotations (---@class, ---@field) for every class and struct: <reference>/lua/<Package>.lua
	bool EmitLua(const Model& m, const Options& o, std::string& error);
	// The decompiled UnrealScript of every class: <reference>/script/<Package>/<Class>.uc
	bool EmitScript(const Model& m, const Options& o, std::string& error);
	// What the old SDK has different from the generated one: offsets, sizes, parameter structs
	bool CompareOldSdk(const Model& m, const Options& o, std::string& error);
}
