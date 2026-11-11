// mirage::layout::Live: the layouts the engine itself linked, read from its reflection objects in memory.

#include "runtime/layout.h"

#include "game/uobject.h"

#include "sdk/SDK.hpp"

#include <cstring>
#include <unordered_set>

namespace mirage::layout
{
	namespace
	{
		// UProperty (after UField, 0x44): ArrayDim, ElementSize, PropertyFlags (qword), ..., Offset. Found by the save
		// state's calibration against SDK members (player_state.cpp) and the same in every run since.
		constexpr int kArrayDim = 0x44;
		constexpr int kElementSize = 0x48;
		constexpr int kFlags = 0x4C;
		constexpr int kOffset = 0x64;
		constexpr int kReference = 0x84;		// The subclass's first field: BitMask, Struct, PropertyClass, Enum, Inner
		constexpr int kFunctionFlags = 0x90;	// UFunction (iNative follows at 0x94: runtime/call.cpp)

		std::uint32_t Dword(const void* p, int at)
		{
			std::uint32_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
			return v;
		}

		Classes::UObject* ObjectAt(const void* p, int at)
		{
			return reinterpret_cast<Classes::UObject*>(static_cast<std::uintptr_t>(Dword(p, at)));
		}

		std::string Path(const Classes::UObject* o)
		{
			std::string out;
			for (int depth = 0; o && depth < 16; o = o->Outer, ++depth)
				out = out.empty() ? o->GetName() : o->GetName() + "." + out;
			return out;
		}

		std::string Package(const Classes::UObject* o)
		{
			while (o && o->Outer) o = o->Outer;
			return o ? o->GetName() : std::string();
		}

		void ReadType(Classes::UField* f, Kind kind, std::string& type, std::string& path)
		{
			// ClassProperty: PropertyClass (always Class), then MetaClass, the class it is limited to
			Classes::UObject* ref = ObjectAt(f, kind == Kind::Class ? kReference + 4 : kReference);
			if (!ref) return;
			type = ref->GetName();
			path = Path(ref);
		}

		Property ReadProperty(Classes::UField* f)
		{
			Property p;
			p.name = f->GetName();
			p.kind = KindFromClass(f->Class ? f->Class->GetName() : "");
			p.arrayDim = Dword(f, kArrayDim);
			p.elementSize = Dword(f, kElementSize);
			std::uint64_t flags;
			std::memcpy(&flags, reinterpret_cast<const std::uint8_t*>(f) + kFlags, sizeof flags);
			p.flags = flags;
			p.offset = Dword(f, kOffset);
			switch (p.kind) {
				case Kind::Bool:
					p.boolMask = Dword(f, kReference);
					break;
				case Kind::Struct: case Kind::Object: case Kind::Class: case Kind::Component: case Kind::Byte:
				case Kind::Interface:
					ReadType(f, p.kind, p.type, p.typePath);
					break;
				case Kind::Array:
					if (auto* inner = reinterpret_cast<Classes::UField*>(ObjectAt(f, kReference)); inner && inner->Class) {
						p.innerKind = KindFromClass(inner->Class->GetName());
						switch (p.innerKind) {	// Only these have a type after UProperty: the rest end there
							case Kind::Struct: case Kind::Object: case Kind::Class: case Kind::Component: case Kind::Byte:
							case Kind::Interface:
								ReadType(inner, p.innerKind, p.innerType, p.innerPath);
								break;
							default:
								break;
						}
					}
					break;
				default:
					break;
			}
			return p;
		}
	}

	std::vector<Property> LiveProperties(const void* ustruct)
	{
		std::vector<Property> out;
		if (!ustruct) return out;
		auto* s = static_cast<const Classes::UStruct*>(ustruct);
		Classes::UClass* const propertyClass = Classes::UProperty::StaticClass();
		int guard = 0;
		for (Classes::UField* f = game::ChildrenOf(s); f && guard < 100000; f = game::NextOf(f), ++guard)
			if (game::IsA(f, propertyClass)) out.push_back(ReadProperty(f));
		return out;
	}

	Layouts Live(const std::vector<std::string>& packages)
	{
		using namespace Classes;
		const std::unordered_set<std::string> wanted(packages.begin(), packages.end());
		Layouts out;
		const auto& objects = UObject::GetGlobalObjects();
		UClass* const structClass = UStruct::StaticClass();
		for (std::size_t i = 0; i < objects.Num(); ++i) {
			UObject* o = objects.GetByIndex(i);
			if (!o || !o->Class || !game::IsA(o, structClass)) continue;
			if (o->GetName().rfind("Default__", 0) == 0) continue;
			const std::string package = Package(o);
			if (!wanted.count(package)) continue;

			auto* s = static_cast<UStruct*>(o);
			Struct st;
			st.path = Path(o);
			st.kind = o->Class->GetName();
			if (UStruct* super = game::SuperOf(s)) st.super = Path(super);
			st.size = game::PropertySizeOf(s);
			if (st.kind == "Function") st.functionFlags = Dword(s, kFunctionFlags);
			st.properties = LiveProperties(s);
			out.emplace(st.path, std::move(st));
		}
		return out;
	}
}
