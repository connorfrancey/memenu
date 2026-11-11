#include "model.h"

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_set>

namespace sdkgen
{
	namespace
	{
		std::string PackageOf(const std::string& path) { return path.substr(0, path.find('.')); }
		std::string LastOf(const std::string& path) { return path.substr(path.find_last_of('.') + 1); }
		std::string OuterOf(const std::string& path)
		{
			const auto dot = path.find_last_of('.');
			return dot == std::string::npos ? std::string() : path.substr(0, dot);
		}
		std::uint32_t Align(std::uint32_t v, std::uint32_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

		void ReadEnums(Model& m, const std::filesystem::path& cooked, const std::vector<std::string>& packages)
		{
			using namespace UnrealLib;
			Linker linker;
			linker.add_search_path(cooked, false);
			ReflectionSchema schema(linker);
			for (const std::string& name : packages) {
				const Package* p = linker.find_package(name);
				if (!p) continue;
				for (std::size_t i = 0; i < p->exports().size(); ++i) {
					const ObjectIndex index = ObjectIndex::from_export(i);
					if (p->class_name(index) != "Enum") continue;
					const FieldObject* f = schema.field(ObjectRef{p, index});
					if (!f) continue;
					Enum e;
					e.path = name + "." + p->object_path(index);
					e.package = name;
					e.name = p->object_name(index);
					for (const auto& v : f->enum_names) e.values.push_back(p->names().to_string(v));
					m.enums.emplace(e.path, std::move(e));
				}
			}
		}
	}

	const Type* Model::TypeAt(const std::string& path) const
	{
		const auto it = types.find(path);
		return it == types.end() ? nullptr : &it->second;
	}

	const Enum* Model::EnumAt(const std::string& path) const
	{
		const auto it = enums.find(path);
		return it == enums.end() ? nullptr : &it->second;
	}

	std::vector<const Property*> Model::Members(const Type& t) const
	{
		std::vector<const Property*> out;
		if (!t.isClass && !t.inherits && !t.layout->super.empty())
			if (const Type* super = TypeAt(t.layout->super)) out = Members(*super);
		for (const Property& p : t.layout->properties) out.push_back(&p);
		return out;
	}

	std::string Identifier(const std::string& name)
	{
		static const std::unordered_set<std::string> reserved = {"alignas", "alignof", "and", "and_eq", "asm", "auto",
			"bitand", "bitor", "bool", "break", "case", "catch", "char", "class", "compl", "concept", "const",
			"consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield",
			"decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
			"extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
			"new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
			"register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
			"static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
			"try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
			"wchar_t", "while", "xor", "xor_eq", "IN", "OUT", "TRUE", "FALSE", "DELETE", "ERROR", "min", "max",
			"near", "far", "PURE", "CONST", "VOID", "INT", "FLOAT", "BOOL", "BYTE", "WORD", "DWORD", "UINT"};
		return reserved.count(name) ? name + "_" : name;
	}

	std::string CppType(const Model& m, const Property& p, bool param)
	{
		const auto objectType = [&](const std::string& path) {
			const Type* t = m.TypeAt(path);
			return "class " + (t && t->isClass ? t->cpp : std::string("UObject")) + "*";
		};
		const auto single = [&](Kind kind, const std::string& path) -> std::string {
			switch (kind) {
				case Kind::Byte: {
					const Enum* e = m.EnumAt(path);
					return e ? "TEnumAsByte<" + e->cpp + ">" : "unsigned char";
				}
				case Kind::Int: return "int";
				case Kind::Float: return "float";
				case Kind::Bool: return param ? "bool" : "unsigned long";
				case Kind::Object: case Kind::Component: return objectType(path);
				case Kind::Class: return "class UClass*";
				case Kind::Name: return "struct FName";
				case Kind::Str: return "struct FString";
				case Kind::Delegate: return "struct FScriptDelegate";
				case Kind::Interface: {
					const Type* t = m.TypeAt(path);
					return t ? "TScriptInterface<class " + t->cpp + ">" : "FScriptInterface";
				}
				case Kind::Struct: {
					const Type* t = m.TypeAt(path);
					return t ? "struct " + t->cpp : "";
				}
				case Kind::Map: return "struct FScriptMap";
				case Kind::Pointer: return "void*";
				default: return "";
			}
		};
		if (p.kind == Kind::Array) {
			if (p.innerKind == Kind::Bool) return "TArray<unsigned long>";
			const std::string inner = single(p.innerKind, p.innerPath);
			return inner.empty() ? "" : "TArray<" + inner + ">";
		}
		return single(p.kind, p.typePath);
	}

	bool Build(Model& m, const std::filesystem::path& cooked, const std::filesystem::path& engineLayouts,
			   std::string& error)
	{
		const auto& all = mirage::layout::SdkPackages();
		std::vector<std::string> scripts;
		for (const auto& name : all)
			if (std::filesystem::exists(cooked / (name + ".u"))) scripts.push_back(name);
		if (scripts.empty()) {
			error = "no script packages in " + cooked.string();
			return false;
		}

		// Both sources: the packages first, then what only the engine knows
		mirage::layout::Computed computed = mirage::layout::Compute(cooked, scripts);
		if (!computed.error.empty()) {
			error = "UnrealLib: " + computed.error;
			return false;
		}
		m.layouts = std::move(computed.layouts);
		std::ifstream in(engineLayouts, std::ios::binary);
		if (!in) {
			error = "can't read " + engineLayouts.string();
			return false;
		}
		std::stringstream text;
		text << in.rdbuf();
		std::string jsonError;
		mirage::layout::Layouts engine = mirage::layout::FromJson(text.str(), &jsonError);
		if (!jsonError.empty()) {
			error = engineLayouts.string() + ": " + jsonError;
			return false;
		}
		std::set<std::string> fromEngine;
		for (auto& [path, s] : engine)
			if (!m.layouts.count(path)) {
				fromEngine.insert(path);
				m.layouts.emplace(path, std::move(s));
			}
		for (const auto& [path, s] : m.layouts)
			if (!s.complete) m.warnings.push_back(path + ": incomplete layout (" + s.note + ")");

		ReadEnums(m, cooked, scripts);

		// The types, and the functions of each class
		const std::set<std::string> wanted(all.begin(), all.end());
		for (const auto& [path, s] : m.layouts) {
			if (!wanted.count(PackageOf(path))) continue;
			if (s.kind != "Class" && s.kind != "ScriptStruct") continue;
			Type t;
			t.path = path;
			t.package = PackageOf(path);
			t.name = LastOf(path);
			t.isClass = s.kind == "Class";
			t.layout = &s;
			t.fromEngine = fromEngine.count(path) > 0;
			m.types.emplace(path, std::move(t));
		}
		for (const auto& [path, s] : m.layouts)
			if (s.kind == "Function")
				if (auto it = m.types.find(OuterOf(path)); it != m.types.end() && it->second.isClass)
					it->second.functions.push_back(&s);

		// C++ names. Classes: A for actors, U for the rest. Structs: F, and enums as they are, both prefixed with
		// their outer class when two share a name (the old SDK's convention: AInterpActor_FCheckpointRecord).
		const std::function<bool(const std::string&)> isActor = [&](const std::string& path) {
			int depth = 0;
			for (std::string p = path; !p.empty() && depth < 64; ++depth) {
				if (p == "Engine.Actor") return true;
				const auto it = m.layouts.find(p);
				if (it == m.layouts.end()) return false;
				p = it->second.super;
			}
			return false;
		};
		std::map<std::string, int> structNames, enumNames;
		for (const auto& [path, t] : m.types)
			if (!t.isClass) ++structNames[t.name];
		for (const auto& [path, e] : m.enums) ++enumNames[e.name];
		for (auto& [path, t] : m.types)
			if (t.isClass) t.cpp = (isActor(path) ? "A" : "U") + t.name;
		const auto outerCpp = [&](const std::string& path) {
			const Type* outer = m.TypeAt(OuterOf(path));
			return outer ? outer->cpp : PackageOf(path);
		};
		for (auto& [path, t] : m.types)
			if (!t.isClass) t.cpp = structNames[t.name] > 1 ? outerCpp(path) + "_F" + t.name : "F" + t.name;
		for (auto& [path, e] : m.enums) e.cpp = enumNames[e.name] > 1 ? outerCpp(path) + "_" + e.name : e.name;
		{
			std::map<std::string, std::string> taken;
			for (const auto& [path, t] : m.types)
				if (auto [it, fresh] = taken.emplace(t.cpp, path); !fresh)
					m.warnings.push_back("two types named " + t.cpp + ": " + it->second + " and " + path);
		}

		// Struct alignment and element size (the computed layouts carry the alignment; engine-only ones get it here,
		// by the same rules: byte 1, the rest 4, structs at least 4, Matrix 16)
		std::function<std::uint32_t(Type&)> alignOf = [&](Type& t) -> std::uint32_t {
			if (t.elementSize || t.isClass) return t.alignment;
			std::uint32_t a = t.layout->alignment;
			if (!a) {
				a = 4;
				for (const Property& p : t.layout->properties)
					if (p.kind == Kind::Struct)
						if (auto it = m.types.find(p.typePath); it != m.types.end()) a = std::max(a, alignOf(it->second));
				if (t.path == "Core.Object.Matrix") a = 16;
			}
			t.alignment = std::max<std::uint32_t>(a, 4);
			t.elementSize = Align(t.layout->size, t.alignment);
			return t.alignment;
		};
		for (auto& [path, t] : m.types) alignOf(t);

		// C++ inheritance where the layout allows it: always for classes (their sizes aren't padded, and the SDK is
		// packed), for a struct only when its super isn't padded (else the struct's first member sits inside the
		// super's padding, which C++ can't express: the super's members are repeated instead)
		for (auto& [path, t] : m.types) {
			std::string p = t.layout->super;
			for (int depth = 0; !p.empty() && !m.types.count(p) && depth < 64; ++depth) {
				const auto it = m.layouts.find(p);
				p = it == m.layouts.end() ? std::string() : it->second.super;
			}
			// The chain broke in a package that isn't read at all: every class is still an Object
			if (p.empty() && t.isClass && !t.layout->super.empty() && path != "Core.Object") p = "Core.Object";
			t.base = p;
		}
		for (auto& [path, t] : m.types) {
			const Type* super = t.base.empty() ? nullptr : m.TypeAt(t.base);
			if (!super) continue;
			t.inherits = t.isClass || super->elementSize == super->layout->size;
		}

		// Header order: a package after every package whose types it holds by value or extends
		std::map<std::string, std::set<std::string>> needs;
		for (const auto& [path, t] : m.types) {
			if (t.inherits) needs[t.package].insert(PackageOf(t.base));
			for (const Property* p : m.Members(t))
				if (p->kind == Kind::Struct) needs[t.package].insert(PackageOf(p->typePath));
		}
		std::vector<std::string> remaining;
		for (const auto& name : all)
			if (std::any_of(m.types.begin(), m.types.end(), [&](const auto& kv) { return kv.second.package == name; }))
				remaining.push_back(name);
		while (!remaining.empty()) {
			const auto ready = std::find_if(remaining.begin(), remaining.end(), [&](const std::string& name) {
				for (const std::string& dep : needs[name])
					if (dep != name && std::find(remaining.begin(), remaining.end(), dep) != remaining.end()) return false;
				return true;
			});
			if (ready == remaining.end()) {
				error = "the packages hold each other's types by value: " + remaining.front();
				return false;
			}
			m.packages.push_back(*ready);
			remaining.erase(ready);
		}
		return true;
	}
}
