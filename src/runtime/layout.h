#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Memory layouts of the game's classes, structs and functions, two ways (Mirage, step 2):
//
//   Live()      read from the engine's own reflection objects in memory (UProperty::Offset, ...): what is true
//   Compute()   worked out from the script packages with UnrealLib, the way the engine links them
//
// Compare() puts them side by side. When the two agree for everything, the computed layouts can be trusted where the
// live ones aren't available: generating the SDK (step 3), and checking a mod's expectations before the game runs.
namespace mirage::layout
{
	enum class Kind : std::uint8_t
	{
		Byte, Int, Bool, Float, Object, Class, Name, Delegate, Interface, Struct, Str, Array, Map, Component, Pointer,
		Unknown,
	};
	const char* KindName(Kind k);
	Kind KindFromClass(const std::string& propertyClass);	// "IntProperty" -> Int

	struct Property
	{
		std::string name;
		Kind kind = Kind::Unknown;
		std::uint32_t offset = 0;
		std::uint32_t elementSize = 0;
		std::uint32_t arrayDim = 1;
		std::uint32_t boolMask = 0;			// Bool: the bit in its dword
		std::uint64_t flags = 0;			// CPF_* (Parm 0x80, OutParm 0x100, ReturnParm 0x400, ...)
		std::string type;					// Struct, class or enum it refers to ("" if none)
		std::string typePath;				// ... its full path ("Core.Object.Vector", "Engine.Actor")
		// Array: the element (its kind, and the struct, class or enum it refers to). Interface: type is the interface.
		Kind innerKind = Kind::Unknown;
		std::string innerType, innerPath;
	};

	struct Struct
	{
		std::string path;					// "Engine.Actor", "Core.Object.Vector", "Engine.Actor.Tick"
		std::string kind;					// "Class", "ScriptStruct", "Function", "State"
		std::string super;					// Path of the struct it extends ("" if none)
		std::uint32_t size = 0;				// PropertySize
		std::uint32_t alignment = 0;		// Computed only (the engine's MinAlignment is not read)
		std::uint32_t functionFlags = 0;	// Function: FUNC_* (Static 0x2000, Native 0x400, Event 0x800, ...)
		std::vector<Property> properties;	// Its own, in link order
		bool complete = true;				// Computed: every property's type was known (false: offsets after it guessed)
		std::string note;					// Computed: why not complete
	};

	using Layouts = std::map<std::string, Struct>;		// By path

	// The engine's layouts of every class, struct, function and state in these packages. Game thread.
	Layouts Live(const std::vector<std::string>& packages);
	// The properties a live UStruct (class, struct, function) declares itself, in link order. Game thread.
	std::vector<Property> LiveProperties(const void* ustruct);

	// The same from the packages in `cooked` (CookedPC), with UnrealLib. Any thread; takes about a second.
	struct Computed
	{
		Layouts layouts;
		std::string error;					// Not empty: nothing could be computed
	};
	Computed Compute(const std::filesystem::path& cooked, const std::vector<std::string>& packages);

	struct Mismatch
	{
		std::string path;					// Struct
		std::string what;					// "Location: offset 0x1A0 computed, 0x19C live", "size ...", "missing ..."
	};
	struct Report
	{
		std::size_t structs = 0, properties = 0;		// Compared
		std::size_t structsMatching = 0, propertiesMatching = 0;
		std::size_t onlyLive = 0, onlyComputed = 0;	// Structs found on one side only
		std::vector<std::string> onlyLivePaths, onlyComputedPaths;
		std::vector<Mismatch> mismatches;			// The first property that differs in each struct, then sizes
		std::map<std::string, std::size_t> byCause;	// Counted by the kind of the first differing property
	};
	Report Compare(const Layouts& live, const Layouts& computed);

	// The packages the SDK is generated for (all the game loads), in dependency order
	const std::vector<std::string>& SdkPackages();

	// Layouts as JSON and back (FromJson: empty with `error` set when the text isn't valid)
	std::string ToJson(const Layouts& layouts);
	Layouts FromJson(const std::string& text, std::string* error = nullptr);
}
