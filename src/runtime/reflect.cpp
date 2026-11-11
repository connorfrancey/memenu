#include "runtime/reflect.h"

#include "game/uobject.h"
#include "sdk/SDK.hpp"

#include <cstring>
#include <unordered_map>

namespace mirage::reflect
{
	namespace
	{
		// UProperty (checked by the self-test "Mirage layouts" against every property of the game)
		constexpr int kArrayDim = 0x44;
		constexpr int kElementSize = 0x48;
		constexpr int kFlags = 0x4C;
		constexpr int kOffset = 0x64;
		constexpr int kReference = 0x84;	// BitMask, Struct, PropertyClass, ...
		constexpr int kMetaClass = 0x88;	// ClassProperty: the class it is limited to

		std::uint32_t Dword(const void* p, int at)
		{
			std::uint32_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
			return v;
		}

		// Members by type, built on first use. Game thread only, like everything here.
		std::unordered_map<const Classes::UStruct*, std::unordered_map<std::string, Member>> g_types;

		const std::unordered_map<std::string, Member>& MembersOf(Classes::UStruct* type)
		{
			if (auto it = g_types.find(type); it != g_types.end()) return it->second;
			std::unordered_map<std::string, Member> members;
			int depth = 0;
			for (Classes::UStruct* s = type; s && depth < 64; s = game::SuperOf(s), ++depth) {
				int count = 0;
				for (Classes::UField* f = game::ChildrenOf(s); f && count < 4096; f = game::NextOf(f), ++count) {
					if (!f->Class) continue;
					const layout::Kind kind = layout::KindFromClass(f->Class->GetName());
					if (kind == layout::Kind::Unknown) continue;	// A function, a const, an enum, a state...
					Member m;
					m.name = f->GetName();
					m.kind = kind;
					m.offset = Dword(f, kOffset);
					m.elementSize = Dword(f, kElementSize);
					m.arrayDim = Dword(f, kArrayDim);
					std::memcpy(&m.flags, reinterpret_cast<const std::uint8_t*>(f) + kFlags, sizeof m.flags);
					switch (kind) {
						case layout::Kind::Bool: m.boolMask = Dword(f, kReference); break;
						case layout::Kind::Struct: m.structType = game::layout::PtrAt<Classes::UStruct>(f, kReference); break;
						case layout::Kind::Object: case layout::Kind::Component: case layout::Kind::Interface:
							m.objectClass = game::layout::PtrAt<Classes::UClass>(f, kReference);
							break;
						case layout::Kind::Class: m.objectClass = game::layout::PtrAt<Classes::UClass>(f, kMetaClass); break;
						default: break;
					}
					members.emplace(m.name, std::move(m));	// The most derived declaration wins (emplace keeps it)
				}
			}
			return g_types.emplace(type, std::move(members)).first->second;
		}

		bool Is(const Value& v, std::initializer_list<layout::Kind> kinds)
		{
			if (!v) return false;
			for (layout::Kind k : kinds)
				if (v.member->kind == k) return true;
			return false;
		}
	}

	const Member* Find(Classes::UStruct* type, std::string_view name)
	{
		if (!type) return nullptr;
		const auto& members = MembersOf(type);
		const auto it = members.find(std::string(name));
		return it == members.end() ? nullptr : &it->second;
	}

	Value Resolve(Classes::UStruct* type, void* base, std::string_view path)
	{
		Value v;
		auto* address = static_cast<std::uint8_t*>(base);
		while (true) {
			const auto dot = path.find('.');
			const std::string_view part = path.substr(0, dot);
			if (!type || !address) {
				v.error = "nothing to look '" + std::string(part) + "' up in";
				return v;
			}
			const Member* m = Find(type, part);
			if (!m) {
				v.error = "no member '" + std::string(part) + "'";
				return v;
			}
			address += m->offset;
			if (dot == std::string_view::npos) {
				v.member = m;
				v.address = address;
				return v;
			}
			if (m->kind == layout::Kind::Struct) {
				type = m->structType;
			} else if (m->kind == layout::Kind::Object || m->kind == layout::Kind::Component) {
				// Through an object reference: its own properties
				Classes::UObject* o = nullptr;
				std::memcpy(&o, address, sizeof o);
				if (!o) {
					v.error = "'" + std::string(part) + "' is none";
					return v;
				}
				type = o->Class;
				address = reinterpret_cast<std::uint8_t*>(o);
			} else {
				v.error = "'" + std::string(part) + "' has no members";
				return v;
			}
			path.remove_prefix(dot + 1);
		}
	}

	std::int32_t GetInt(const Value& v)
	{
		if (Is(v, {layout::Kind::Int})) {
			std::int32_t x;
			std::memcpy(&x, v.address, sizeof x);
			return x;
		}
		if (Is(v, {layout::Kind::Byte})) return *v.address;
		if (Is(v, {layout::Kind::Bool})) return (Dword(v.address, 0) & v.member->boolMask) ? 1 : 0;
		if (Is(v, {layout::Kind::Float})) return static_cast<std::int32_t>(GetFloat(v));
		return 0;
	}

	float GetFloat(const Value& v)
	{
		if (Is(v, {layout::Kind::Float})) {
			float x;
			std::memcpy(&x, v.address, sizeof x);
			return x;
		}
		if (Is(v, {layout::Kind::Int, layout::Kind::Byte, layout::Kind::Bool})) return static_cast<float>(GetInt(v));
		return 0.f;
	}

	Classes::UObject* GetObj(const Value& v)
	{
		if (!Is(v, {layout::Kind::Object, layout::Kind::Class, layout::Kind::Component, layout::Kind::Interface})) return nullptr;
		Classes::UObject* o = nullptr;
		std::memcpy(&o, v.address, sizeof o);
		return o;
	}

	std::string GetText(const Value& v)
	{
		if (Is(v, {layout::Kind::Name})) return reinterpret_cast<const Classes::FName*>(v.address)->GetName();
		if (Is(v, {layout::Kind::Str})) {
			const auto* s = reinterpret_cast<const Classes::FString*>(v.address);
			return s->IsValid() ? s->ToString() : std::string();
		}
		if (Is(v, {layout::Kind::Float})) return std::to_string(GetFloat(v));
		if (Is(v, {layout::Kind::Int, layout::Kind::Byte, layout::Kind::Bool})) return std::to_string(GetInt(v));
		if (Classes::UObject* o = GetObj(v)) return NameOf(o);
		return {};
	}

	bool SetInt(const Value& v, std::int32_t x)
	{
		if (Is(v, {layout::Kind::Int})) {
			std::memcpy(v.address, &x, sizeof x);
			return true;
		}
		if (Is(v, {layout::Kind::Byte})) {
			*v.address = static_cast<std::uint8_t>(x);
			return true;
		}
		if (Is(v, {layout::Kind::Bool})) {
			std::uint32_t d = Dword(v.address, 0);
			d = x ? (d | v.member->boolMask) : (d & ~v.member->boolMask);
			std::memcpy(v.address, &d, sizeof d);
			return true;
		}
		if (Is(v, {layout::Kind::Float})) return SetFloat(v, static_cast<float>(x));
		return false;
	}

	bool SetFloat(const Value& v, float x)
	{
		if (Is(v, {layout::Kind::Float})) {
			std::memcpy(v.address, &x, sizeof x);
			return true;
		}
		if (Is(v, {layout::Kind::Int, layout::Kind::Byte, layout::Kind::Bool})) return SetInt(v, static_cast<std::int32_t>(x));
		return false;
	}

	bool SetObj(const Value& v, Classes::UObject* x)
	{
		if (!Is(v, {layout::Kind::Object, layout::Kind::Class, layout::Kind::Component})) return false;
		std::memcpy(v.address, &x, sizeof x);
		return true;
	}

	bool SetText(const Value& v, std::string_view x)
	{
		if (!Is(v, {layout::Kind::Name})) return false;
		const Classes::FName name(std::string(x).c_str());
		if (name.Index == 0 && x != "None") return false;		// Not a name the engine has
		std::memcpy(v.address, &name, sizeof name);
		return true;
	}

	void ClearCache() { g_types.clear(); }

	std::string NameOf(const Classes::UObject* object) { return object ? object->GetName() : std::string("None"); }
	std::string ClassNameOf(const Classes::UObject* object)
	{
		return object && object->Class ? object->Class->GetName() : std::string();
	}
}
