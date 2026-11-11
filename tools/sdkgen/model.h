#pragma once
// mirage_sdkgen: the game's types as the generated SDK sees them (Mirage, step 3).
//
// Built from two sources: the layouts computed from the script packages with UnrealLib (mirage::layout::Compute, the
// same code the in-game self-test proves against the engine) and the layouts only the engine knows (intrinsic classes,
// native-only packages), saved by that self-test to engine-layouts.json. Enums come from the packages.

#include "runtime/layout.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sdkgen
{
	using mirage::layout::Kind;
	using mirage::layout::Property;
	using mirage::layout::Struct;

	struct Enum
	{
		std::string path;					// "Engine.Actor.EPhysics"
		std::string package, name;
		std::string cpp;					// "EPhysics" (or "AActor_EPhysics" when the name is taken)
		std::vector<std::string> values;
	};

	struct Type							// A class or script struct
	{
		std::string path, package, name;
		std::string cpp;					// "AActor", "FVector", "AInterpActor_FCheckpointRecord"
		bool isClass = false;
		const Struct* layout = nullptr;
		bool fromEngine = false;			// Only the engine knows it (engine-layouts.json)
		std::uint32_t alignment = 4;
		std::uint32_t elementSize = 0;		// Struct: its size padded to its alignment (what a property holding it takes)
		std::string base;					// The nearest super the SDK has ("" if none): the super itself, unless
											// it is in a package the SDK leaves out (ExampleBuilder: Editor.BrushBuilder)
		bool inherits = false;				// C++ inheritance from `base` (else its members are repeated)
		std::vector<const Struct*> functions;	// Class: the functions it declares
	};

	struct Model
	{
		mirage::layout::Layouts layouts;	// Everything, both sources
		std::map<std::string, Type> types;	// Classes and script structs by path
		std::map<std::string, Enum> enums;	// By path
		std::vector<std::string> packages;	// In the order their headers include each other
		std::vector<std::string> warnings;

		const Type* TypeAt(const std::string& path) const;
		const Enum* EnumAt(const std::string& path) const;
		// The members of `t` the C++ type declares: its own, or, when it doesn't inherit, its supers' too
		std::vector<const Property*> Members(const Type& t) const;
	};

	// False with `error` when the packages can't be read
	bool Build(Model& model, const std::filesystem::path& cooked, const std::filesystem::path& engineLayouts,
			   std::string& error);

	// What C++ calls the type of `p`: "float", "struct FVector", "TArray<class AActor*>", "TEnumAsByte<EPhysics>"
	std::string CppType(const Model& m, const Property& p, bool param);
	// A safe C++ identifier for an UnrealScript name (C++ keywords get a trailing underscore)
	std::string Identifier(const std::string& name);
}
