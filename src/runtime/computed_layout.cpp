// mirage::layout::Compute and Compare: layouts worked out from the script packages with UnrealLib, and how they line up
// with the engine's.
//
// The rules (UE3's UStruct::Link, as the comparison with the live engine showed them; see TestMirageLayouts):
//   - a class or script struct starts where its super ends (its PropertySize); a function or state starts at 0
//   - properties are laid out in children-list order, each aligned to its type: byte 1, the rest 4, a struct to its
//     alignment, which is at least 4 (Color, four bytes, sits on a dword) and 16 for Matrix (the native FMatrix is
//     16-byte aligned)
//   - consecutive bools share a dword (BitMask 1, 2, 4, ...), except in functions: each parameter gets its own
//   - sizes are not rounded up: Box (two vectors and a byte) is 0x19 bytes, and a class ends at its last property;
//     a struct property is, though: its element size is the struct's size rounded up to its alignment (Box: 0x1C)
//   - a map property is 60 bytes (the native TMap), found only in the engine (packages don't say)

#include "runtime/layout.h"

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace mirage::layout
{
	const char* KindName(Kind k)
	{
		static const char* const kNames[] = {"Byte", "Int", "Bool", "Float", "Object", "Class", "Name", "Delegate",
			"Interface", "Struct", "Str", "Array", "Map", "Component", "Pointer", "Unknown"};
		return kNames[static_cast<int>(k)];
	}

	Kind KindFromClass(const std::string& c)
	{
		static const std::unordered_map<std::string, Kind> kinds = {
			{"ByteProperty", Kind::Byte}, {"IntProperty", Kind::Int}, {"BoolProperty", Kind::Bool},
			{"FloatProperty", Kind::Float}, {"ObjectProperty", Kind::Object}, {"ClassProperty", Kind::Class},
			{"NameProperty", Kind::Name}, {"DelegateProperty", Kind::Delegate}, {"InterfaceProperty", Kind::Interface},
			{"StructProperty", Kind::Struct}, {"StrProperty", Kind::Str}, {"ArrayProperty", Kind::Array},
			{"MapProperty", Kind::Map}, {"ComponentProperty", Kind::Component}, {"PointerProperty", Kind::Pointer},
		};
		const auto it = kinds.find(c);
		return it == kinds.end() ? Kind::Unknown : it->second;
	}

	namespace
	{
		using namespace UnrealLib;

		std::uint32_t Align(std::uint32_t v, std::uint32_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

		Kind FromType(PropertyType t)
		{
			switch (t) {
				case PropertyType::Byte: return Kind::Byte;
				case PropertyType::Int: return Kind::Int;
				case PropertyType::Bool: return Kind::Bool;
				case PropertyType::Float: return Kind::Float;
				case PropertyType::Object: return Kind::Object;
				case PropertyType::Class: return Kind::Class;
				case PropertyType::Name: return Kind::Name;
				case PropertyType::Delegate: return Kind::Delegate;
				case PropertyType::Interface: return Kind::Interface;
				case PropertyType::Struct: return Kind::Struct;
				case PropertyType::Str: return Kind::Str;
				case PropertyType::Array: return Kind::Array;
				case PropertyType::Map: return Kind::Map;
				case PropertyType::Component: return Kind::Component;
				case PropertyType::Pointer: return Kind::Pointer;
				default: return Kind::Unknown;
			}
		}

		class Calculator
		{
		public:
			Calculator(Linker& linker, ReflectionSchema& schema) : linker_(linker), schema_(schema) {}

			static std::string PathOf(ObjectRef ref) { return ref.package->name() + "." + ref.package->object_path(ref.index); }

			// The layout of a class, struct, function or state (cached; nullptr if it can't be read)
			const Struct* Of(ObjectRef ref)
			{
				const auto key = std::make_pair(ref.package, ref.index.value);
				if (auto it = cache_.find(key); it != cache_.end()) return it->second.get();
				if (!inProgress_.insert(key).second) return nullptr;	// A struct containing itself: not possible, but guard
				auto st = Build(ref);
				inProgress_.erase(key);
				return cache_.emplace(key, std::move(st)).first->second.get();
			}

		private:
			std::optional<ObjectRef> Resolve(const Package& p, ObjectIndex index)
			{
				if (index.is_null()) return std::nullopt;
				if (index.is_export()) return ObjectRef{&p, index};
				const Resolution& r = linker_.resolve(p, index);
				if (r.status == ResolveStatus::Export) return r.target;
				return std::nullopt;
			}

			std::unique_ptr<Struct> Build(ObjectRef ref)
			{
				const FieldObject* f = schema_.field(ref);
				if (!f || !f->structure) return nullptr;
				auto st = std::make_unique<Struct>();
				st->path = PathOf(ref);
				st->kind = ref.package->class_name(ref.index);
				const bool inherits = st->kind == "Class" || st->kind == "ScriptStruct" || st->kind == "Struct";
				const bool isFunction = st->kind == "Function";

				std::uint32_t offset = 0, align = 1;
				if (auto super = Resolve(*ref.package, f->field.super)) {
					st->super = PathOf(*super);
					if (inherits) {
						if (const Struct* s = Of(*super)) {
							offset = s->size;
							align = std::max(align, s->alignment);
							if (!s->complete) {
								st->complete = false;
								st->note = "its super " + s->path + " is incomplete";
							}
						} else {
							st->complete = false;
							st->note = "its super " + st->super + " could not be read";
						}
					}
				}

				Property* prevBool = nullptr;
				int guard = 0;
				for (ObjectIndex c = f->structure->children; !c.is_null() && guard < 100000; ++guard) {
					const FieldObject* child = schema_.field(ObjectRef{ref.package, c});
					if (!child) {
						st->complete = false;
						st->note = "a field could not be read";
						break;
					}
					const ObjectIndex next = child->field.next;
					if (child->kind != FieldKind::Property || !child->property) {
						c = next;
						continue;
					}
					const PropertyData& data = *child->property;
					Property p;
					p.name = ref.package->object_name(c);
					p.kind = FromType(data.type);
					p.arrayDim = static_cast<std::uint32_t>(std::max(1, data.dimension()));
					p.flags = data.flags;

					std::uint32_t size = 4, a = 4;
					switch (p.kind) {
						case Kind::Byte: size = 1; a = 1; break;
						case Kind::Name: size = 8; break;
						case Kind::Interface: size = 8; break;
						case Kind::Delegate: size = 12; break;
						case Kind::Str: case Kind::Array: size = 12; break;
						case Kind::Struct: {
							const auto target = Resolve(*ref.package, data.reference);
							const Struct* s = target ? Of(*target) : nullptr;
							if (target) p.type = target->package->object_name(target->index);
							if (!s) {
								st->complete = false;
								st->note = p.name + ": its struct could not be read";
								size = 0;
							} else {
								a = std::max<std::uint32_t>(1, s->alignment);
								size = Align(s->size, a);	// The struct isn't padded, a property holding it is
								if (!s->complete && st->complete) {
									st->complete = false;
									st->note = p.name + ": " + s->path + " is incomplete";
								}
							}
							break;
						}
						case Kind::Map: size = 60; break;
						case Kind::Unknown:
							st->complete = false;
							st->note = p.name + ": the size of a " + KindName(p.kind) + " property is not known";
							size = 0;
							break;
						default: break;
					}
					if ((p.kind == Kind::Object || p.kind == Kind::Class || p.kind == Kind::Component ||
						 p.kind == Kind::Byte) && !data.reference.is_null())
						if (const auto target = Resolve(*ref.package, data.reference))
							p.type = target->package->object_name(target->index);

					if (p.kind == Kind::Bool) {
						p.elementSize = 4;
						if (!isFunction && prevBool && prevBool->arrayDim == 1 && p.arrayDim == 1 &&
							(prevBool->boolMask << 1) != 0) {
							p.offset = prevBool->offset;
							p.boolMask = prevBool->boolMask << 1;
						} else {
							offset = Align(offset, 4);
							p.offset = offset;
							p.boolMask = 1;
							offset += 4 * p.arrayDim;
						}
					} else {
						p.elementSize = size;
						offset = Align(offset, a);
						p.offset = offset;
						offset += size * p.arrayDim;
					}
					align = std::max(align, a);
					st->properties.push_back(std::move(p));
					prevBool = st->properties.back().kind == Kind::Bool ? &st->properties.back() : nullptr;
					c = next;
				}
				const bool isStruct = st->kind == "ScriptStruct" || st->kind == "Struct";
				st->alignment = isStruct ? std::max<std::uint32_t>(align, 4) : align;
				if (st->path == "Core.Object.Matrix") st->alignment = 16;
				st->size = offset;
				return st;
			}

			Linker& linker_;
			ReflectionSchema& schema_;
			std::map<std::pair<const Package*, std::int32_t>, std::unique_ptr<Struct>> cache_;
			std::set<std::pair<const Package*, std::int32_t>> inProgress_;
		};
	}

	Computed Compute(const std::filesystem::path& cooked, const std::vector<std::string>& packages)
	{
		Computed out;
		try {
			Linker linker;
			linker.add_search_path(cooked, false);
			ReflectionSchema schema(linker);
			std::vector<const Package*> loaded;
			for (const std::string& name : packages) {
				const Package* p = linker.find_package(name);
				if (!p) throw std::runtime_error(name + ".u was not found in " + cooked.string());
				schema.add_package(*p);
				loaded.push_back(p);
			}
			Calculator calc(linker, schema);
			for (const Package* p : loaded)
				for (std::size_t i = 0; i < p->exports().size(); ++i) {
					const ObjectIndex index = ObjectIndex::from_export(i);
					const std::string cls = p->class_name(index);
					if (cls != "Class" && cls != "ScriptStruct" && cls != "Function" && cls != "State") continue;
					if (const Struct* s = calc.Of(ObjectRef{p, index})) out.layouts.emplace(s->path, *s);
				}
		} catch (const std::exception& e) {
			out.error = e.what();
		}
		return out;
	}

	Report Compare(const Layouts& live, const Layouts& computed)
	{
		Report r;
		for (const auto& [path, l] : live)
			if (!computed.count(path)) {
				++r.onlyLive;
				r.onlyLivePaths.push_back(path + " (" + l.kind + ")");
			}
		for (const auto& [path, c] : computed) {
			const auto it = live.find(path);
			if (it == live.end()) {
				++r.onlyComputed;
				r.onlyComputedPaths.push_back(path + " (" + c.kind + ")");
				continue;
			}
			const Struct& l = it->second;
			++r.structs;
			std::unordered_map<std::string, const Property*> byName;
			for (const Property& p : l.properties) byName.emplace(p.name, &p);

			bool same = true;
			for (const Property& cp : c.properties) {
				++r.properties;
				const auto lp = byName.find(cp.name);
				std::string what;
				if (lp == byName.end()) what = cp.name + ": not in the engine";
				else {
					const Property& p = *lp->second;
					if (p.offset != cp.offset)
						what = fmt::format("{}: offset {:#x} computed, {:#x} live", cp.name, cp.offset, p.offset);
					else if (p.elementSize != cp.elementSize)
						what = fmt::format("{}: size {} computed, {} live", cp.name, cp.elementSize, p.elementSize);
					else if (p.arrayDim != cp.arrayDim)
						what = fmt::format("{}: array of {} computed, {} live", cp.name, cp.arrayDim, p.arrayDim);
					else if (p.kind == Kind::Bool && p.boolMask != cp.boolMask)
						what = fmt::format("{}: bit {:#x} computed, {:#x} live", cp.name, cp.boolMask, p.boolMask);
				}
				if (what.empty()) {
					++r.propertiesMatching;
					continue;
				}
				if (same) {		// The first difference: what follows usually just follows from it
					const std::string type = cp.kind == Kind::Struct ? "Struct " + cp.type : KindName(cp.kind);
					r.mismatches.push_back({path, what + " (" + type + (c.complete ? "" : "; " + c.note) + ")"});
					++r.byCause[std::string(c.kind) + " / " + type];
				}
				same = false;
			}
			if (same && l.properties.size() != c.properties.size()) {
				same = false;
				r.mismatches.push_back({path, fmt::format("{} properties computed, {} live", c.properties.size(),
														  l.properties.size())});
				++r.byCause[c.kind + " / property count"];
			}
			if (same && l.size != c.size) {
				same = false;
				r.mismatches.push_back({path, fmt::format("size {:#x} computed, {:#x} live (alignment {})", c.size,
														  l.size, c.alignment)});
				++r.byCause[c.kind + " / size"];
			}
			if (same) ++r.structsMatching;
		}
		return r;
	}
}
