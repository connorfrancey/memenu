#pragma once
// Game objects by name (Mirage, step 6): a property of an object, a member of a struct in it, a parameter of a call,
// found through the engine's own UProperty objects and a dotted path ("Location.Z", "Velocity", "Controller").
//
//     reflect::Value v = reflect::Resolve(pawn->Class, pawn, "Location.Z");
//     float z = reflect::GetFloat(v);
//     reflect::SetFloat(v, z + 100.f);
//
// What the menu's mods reach game objects with (MirageApi object_* / call_*, the `mirage` Lua table). Game thread.

#include "runtime/layout.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Classes
{
	class UObject;
	class UStruct;
	class UClass;
}

namespace mirage::reflect
{
	struct Member
	{
		std::string name;
		layout::Kind kind = layout::Kind::Unknown;
		std::uint32_t offset = 0, elementSize = 0, arrayDim = 1, boolMask = 0;
		std::uint64_t flags = 0;
		Classes::UStruct* structType = nullptr;		// Struct: its UScriptStruct
		Classes::UClass* objectClass = nullptr;		// Object, Class, Component: the class it holds
	};

	// The property `name` of `type` or a struct/class it extends (cached per type). nullptr if there is none.
	const Member* Find(Classes::UStruct* type, std::string_view name);

	// Where a dotted path leads: the member it ends at and its address
	struct Value
	{
		const Member* member = nullptr;
		std::uint8_t* address = nullptr;
		std::string error;							// Why the path doesn't resolve
		explicit operator bool() const { return member && address; }
	};
	// `base` is an instance of `type` (an object and its class, a call's parameters and its function)
	Value Resolve(Classes::UStruct* type, void* base, std::string_view path);

	// Reading and writing a value of the kind the member has. Get on another kind gives 0 / "" / nullptr; Set on
	// another kind does nothing and returns false.
	std::int32_t GetInt(const Value& v);			// Int, Byte, Bool (0/1)
	float GetFloat(const Value& v);					// Float (Int and Byte converted)
	Classes::UObject* GetObject(const Value& v);	// Object, Class, Component, Interface
	std::string GetText(const Value& v);			// Name, Str (and a number as text)
	bool SetInt(const Value& v, std::int32_t x);	// Int, Byte (0-255), Bool
	bool SetFloat(const Value& v, float x);
	bool SetObject(const Value& v, Classes::UObject* x);
	bool SetText(const Value& v, std::string_view x);	// Name only (a string's memory belongs to the engine)

	// Forgets the members found so far: classes of a level's packages go with the level. On a level load.
	void ClearCache();

	std::string NameOf(const Classes::UObject* object);			// "TdPlayerPawn_0"
	std::string ClassNameOf(const Classes::UObject* object);		// "TdPlayerPawn"
}
