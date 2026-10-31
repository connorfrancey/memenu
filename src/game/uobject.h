#pragma once
// Reflection of the engine's own objects (classes, their fields), done right for this build of the game.
//
// The SDK (mmultiplayer's generated ME_Core_classes.hpp) has the layout of UField and UStruct wrong:
//
//                    SDK                     the game (UE3 of 2008, checked in game::Init)
//   UField           Next        @ 0x3C      SuperField @ 0x3C, Next @ 0x40         (UField is 0x44 bytes)
//   UStruct          SuperField  @ 0x48      ScriptText @ 0x44, CppText @ 0x48, Children @ 0x4C, PropertySize @ 0x50
//
// So with the SDK's members, `field->Next` reads the field's SuperField (null: a walk over a class's properties stops
// after the first one) and `UObject::IsA()` walks CppText instead of the parent class: it only ever matches the exact
// class (a TdPlayerController is not "a PlayerController"). The SDK's class sizes are left alone (its generated
// function wrappers depend on them); use these instead of IsA() / SuperField / Children / Next.
#include <ME_SDK.hpp>

#include <cstdint>
#include <cstring>

namespace game {
	namespace layout {
		constexpr int kFieldSuper = 0x3C;
		constexpr int kFieldNext = 0x40;
		constexpr int kStructChildren = 0x4C;
		constexpr int kStructPropertySize = 0x50;

		template <class T = void>
		T* PtrAt(const void* object, int offset)
		{
			std::uint32_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(object) + offset, sizeof v);
			return reinterpret_cast<T*>(static_cast<std::uintptr_t>(v));
		}
	}

	// The struct/class this one extends (null for Object).
	inline Classes::UStruct* SuperOf(const Classes::UStruct* s) { return layout::PtrAt<Classes::UStruct>(s, layout::kFieldSuper); }
	// First field declared by this struct/class (its own, not inherited ones), then NextOf() for the rest.
	inline Classes::UField* ChildrenOf(const Classes::UStruct* s) { return layout::PtrAt<Classes::UField>(s, layout::kStructChildren); }
	inline Classes::UField* NextOf(const Classes::UField* f) { return layout::PtrAt<Classes::UField>(f, layout::kFieldNext); }
	// Bytes of an instance of this struct/class.
	inline std::uint32_t PropertySizeOf(const Classes::UStruct* s)
	{
		std::uint32_t v;
		std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(s) + layout::kStructPropertySize, sizeof v);
		return v;
	}

	// `object` is an instance of `cls` or of a class derived from it (what UObject::IsA() should do).
	inline bool IsA(const Classes::UObject* object, const Classes::UClass* cls)
	{
		if (!object || !cls) return false;
		int depth = 0;
		for (const Classes::UStruct* c = object->Class; c && depth < 64; c = SuperOf(c), ++depth)
			if (c == cls) return true;
		return false;
	}

	// The object found at `index` of the engine's object list is still alive: the slot still holds it (the engine
	// clears the slot when it destroys an object). Never dereferences `object`, so it is safe on a dangling pointer.
	inline bool AliveAt(const Classes::UObject* object, std::size_t index)
	{
		const auto& objects = Classes::UObject::GetGlobalObjects();
		return object && index < objects.Num() && objects.GetByIndex(index) == object;
	}

	// Index of a live object in the engine's object list (UObject::ObjectInternalInteger), for AliveAt().
	inline std::size_t IndexOf(const Classes::UObject* object) { return static_cast<std::size_t>(object->ObjectInternalInteger); }

	// Checks the layout above against the game (game::Init calls it): false and an error in the log if it is off.
	bool CheckObjectLayout();
}
