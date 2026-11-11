// mirage_sdkgen: the C++ SDK. One header and one source per package; the headers declare the enums, structs and
// classes with every member at the engine's offset (packed, explicit padding); the sources hold the function wrappers,
// a static_assert for every offset and size, and the bitfield checks the compiler can't do.

#include "emit.h"

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace sdkgen
{
	namespace
	{
		constexpr std::uint64_t kParm = 0x80, kOutParm = 0x100, kReturnParm = 0x400;
		constexpr std::uint32_t kFuncStatic = 0x2000;

		std::uint32_t Align(std::uint32_t v, std::uint32_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }
		int Log2(std::uint32_t v)
		{
			int n = 0;
			while (v > 1) { v >>= 1; ++n; }
			return n;
		}

		// Members the packages don't declare: the reflection classes' native fields, at the offsets the engine was
		// found to use (runtime/live_layout.cpp, runtime/call.cpp, game/uobject.h; checked against the engine by the
		// self-tests "Mirage layouts" and "Mirage calls")
		struct Known
		{
			const char* path;
			const char* cpp;
			const char* name;
			std::uint32_t offset, size;
		};
		const Known kKnown[] = {
			{"Core.Field", "class UStruct*", "SuperField", 0x3C, 4},
			{"Core.Field", "class UField*", "Next", 0x40, 4},
			{"Core.Struct", "class UTextBuffer*", "ScriptText", 0x44, 4},
			{"Core.Struct", "class UTextBuffer*", "CppText", 0x48, 4},
			{"Core.Struct", "class UField*", "Children", 0x4C, 4},
			{"Core.Struct", "int", "PropertySize", 0x50, 4},
			{"Core.Struct", "TArray<unsigned char>", "Script", 0x54, 12},
			{"Core.Function", "unsigned long", "FunctionFlags", 0x90, 4},
			{"Core.Function", "unsigned short", "iNative", 0x94, 2},
			{"Core.Function", "unsigned short", "RepOffset", 0x96, 2},
			{"Core.Function", "struct FName", "FriendlyName", 0x98, 8},
			{"Core.Function", "unsigned char", "OperPrecedence", 0xA0, 1},
			{"Core.Function", "unsigned char", "NumParms", 0xA1, 1},
			{"Core.Property", "int", "ArrayDim", 0x44, 4},
			{"Core.Property", "int", "ElementSize", 0x48, 4},
			{"Core.Property", "unsigned long long", "PropertyFlags", 0x4C, 8},
			{"Core.Property", "int", "Offset", 0x64, 4},
			{"Core.BoolProperty", "unsigned long", "BitMask", 0x84, 4},
			{"Core.ByteProperty", "class UEnum*", "Enum", 0x84, 4},
			{"Core.ObjectProperty", "class UClass*", "PropertyClass", 0x84, 4},
			{"Core.ClassProperty", "class UClass*", "MetaClass", 0x88, 4},
			{"Core.InterfaceProperty", "class UClass*", "InterfaceClass", 0x84, 4},
			{"Core.StructProperty", "class UScriptStruct*", "Struct", 0x84, 4},
			{"Core.ArrayProperty", "class UProperty*", "Inner", 0x84, 4},
			{"Core.DelegateProperty", "class UFunction*", "Function", 0x84, 4},
			{"Core.Enum", "TArray<struct FName>", "Names", 0x44, 12},
		};

		// Names the hand-written UObject members use: a generated function by one of these names is renamed
		const std::set<std::string> kHandWritten = {"GObjects", "GetGlobalObjects", "GetName", "GetObjectName",
			"GetFullName", "GetPathName", "FindNameIndex", "FindObject", "FindClass", "GetObjectCasted", "ProcessEvent",
			"StaticClass"};

		std::string Flags(std::uint64_t f)
		{
			static const std::pair<std::uint64_t, const char*> names[] = {{0x1, "Edit"}, {0x2, "Const"}, {0x20, "Net"},
				{0x80, "Parm"}, {0x100, "OutParm"}, {0x400, "ReturnParm"}, {0x10, "OptionalParm"}, {0x1000, "Native"},
				{0x2000, "Transient"}, {0x4000, "Config"}, {0x80000, "Component"}};
			std::string out;
			for (const auto& [bit, name] : names)
				if (f & bit) out += (out.empty() ? "" : ", ") + std::string(name);
			return out;
		}

		std::string FunctionFlagNames(std::uint32_t f)
		{
			static const std::pair<std::uint32_t, const char*> names[] = {{0x1, "Final"}, {0x2, "Defined"},
				{0x8, "Latent"}, {0x40, "Net"}, {0x100, "Simulated"}, {0x200, "Exec"}, {0x400, "Native"},
				{0x800, "Event"}, {0x1000, "Operator"}, {0x2000, "Static"}, {0x8000, "Const"},
				{0x100000, "Delegate"}};
			std::string out;
			for (const auto& [bit, name] : names)
				if (f & bit) out += (out.empty() ? "" : ", ") + std::string(name);
			return out;
		}

		struct Member
		{
			std::string name, cpp, comment;
			std::uint32_t offset = 0, size = 0, dim = 1;
			bool bit = false;
			std::uint32_t mask = 0;
		};

		struct Checks
		{
			std::vector<std::string> offsets;	// static_assert lines
			std::vector<std::string> bits;		// MIRAGE_BIT lines
		};

		// The body of a struct, class or parameter struct: the members at their offsets, padding between them.
		// `end` is the size the C++ type must have.
		std::string Body(const std::string& owner, std::vector<Member> members, std::uint32_t start, std::uint32_t end,
						 bool bitfields, Checks& checks, std::vector<std::string>& warnings)
		{
			std::stable_sort(members.begin(), members.end(), [](const Member& a, const Member& b) {
				return a.offset != b.offset ? a.offset < b.offset : a.mask < b.mask;
			});
			std::string out;
			std::uint32_t cur = start, groupAt = 0, prevMask = 0;
			bool prevBit = false;
			const auto pad = [&](std::uint32_t to) {
				if (to > cur) out += fmt::format("\tunsigned char UnknownData_{:04X}[0x{:X}];\n", cur, to - cur);
				cur = std::max(cur, to);
			};
			for (const Member& m : members) {
				if (m.bit && bitfields) {
					if (prevBit && m.offset == groupAt && m.mask > prevMask) {
						if (const int gap = Log2(m.mask) - Log2(prevMask) - 1; gap > 0)
							out += fmt::format("\tunsigned long : {};\n", gap);
					} else {
						if (m.offset < cur) {
							warnings.push_back(fmt::format("{}::{} at {:#x} overlaps the member before it", owner, m.name,
														   m.offset));
							continue;
						}
						if (prevBit && m.offset == cur) out += "\tunsigned long : 0;\n";
						pad(m.offset);
						if (const int lead = Log2(m.mask); lead > 0) out += fmt::format("\tunsigned long : {};\n", lead);
						groupAt = m.offset;
						cur = m.offset + 4;
					}
					out += fmt::format("\t{:<50} {:<50} // 0x{:04X} (bit 0x{:X}){}\n", "unsigned long", m.name + " : 1;",
									   m.offset, m.mask, m.comment.empty() ? "" : " (" + m.comment + ")");
					checks.bits.push_back(fmt::format("\tMIRAGE_BIT({}, {}, 0x{:X}, 0x{:X});", owner, m.name, m.offset,
													  m.mask));
					prevMask = m.mask;
					prevBit = true;
					continue;
				}
				if (m.offset < cur) {
					warnings.push_back(fmt::format("{}::{} at {:#x} overlaps the member before it", owner, m.name,
												   m.offset));
					continue;
				}
				pad(m.offset);
				const std::string decl = m.dim > 1 ? fmt::format("{}[{}];", m.name, m.dim) : m.name + ";";
				out += fmt::format("\t{:<50} {:<50} // 0x{:04X} (0x{:04X}){}\n", m.cpp, decl, m.offset, m.size * m.dim,
								   m.comment.empty() ? "" : " (" + m.comment + ")");
				checks.offsets.push_back(fmt::format("MIRAGE_OFFSET({}, {}, 0x{:X});", owner, m.name, m.offset));
				cur = m.offset + m.size * m.dim;
				prevBit = false;
			}
			if (cur > end)
				warnings.push_back(fmt::format("{}: members end at {:#x}, past its size {:#x}", owner, cur, end));
			pad(end);
			return out;
		}

		class Emitter
		{
		public:
			Emitter(const Model& m, const Options& o) : m_(m), o_(o) {}

			bool Run(std::string& error)
			{
				namespace fs = std::filesystem;
				const fs::path include = o_.out / "include" / "sdk", src = o_.out / "src";
				std::error_code ec;
				fs::remove_all(include, ec);
				fs::remove_all(src, ec);
				fs::create_directories(include);
				fs::create_directories(src);

				for (const char* name : {"Basic.hpp"})
					if (!Copy(o_.statics / name, include / name, error)) return false;
				if (!Copy(o_.statics / "Basic.cpp", src / "Basic.cpp", error)) return false;
				if (!Copy(o_.statics / "CMakeLists.txt", o_.out / "CMakeLists.txt", error)) return false;
				if (!Read(o_.statics / "UObject.inl", uobjectExtras_, error)) return false;

				Write(include / "Forward.hpp", Forward());
				std::string umbrella = Banner() + "#pragma once\n\n#include \"sdk/Basic.hpp\"\n#include \"sdk/Forward.hpp\"\n";
				std::string checks = Banner() + "#include \"sdk/SDK.hpp\"\n\nnamespace Classes::detail\n{\n";
				for (const std::string& pkg : m_.packages) {
					Package(pkg);
					umbrella += "#include \"sdk/" + pkg + ".hpp\"\n";
					checks += "\tint CheckLayouts_" + pkg + "(std::string* first);\n";
				}
				checks += "\n\tint CheckLayouts(std::string* first)\n\t{\n\t\tint failures = 0;\n";
				for (const std::string& pkg : m_.packages) checks += "\t\tfailures += CheckLayouts_" + pkg + "(first);\n";
				checks += "\t\treturn failures;\n\t}\n}\n";
				Write(include / "SDK.hpp", umbrella);
				Write(src / "Checks.cpp", checks);
				for (const auto& w : warnings_) std::fprintf(stderr, "warning: %s\n", w.c_str());
				return true;
			}

			std::size_t Classes() const { return classes_; }
			std::size_t Structs() const { return structs_; }
			std::size_t Functions() const { return functions_; }
			std::size_t Skipped() const { return skipped_; }
			const std::vector<std::string>& Warnings() const { return warnings_; }

		private:
			static std::string Banner()
			{
				return "// Mirage SDK for Mirror's Edge (2008, PC). Generated by mirage_sdkgen (tools/sdkgen) from the game's\n"
					   "// packages and the engine's own layouts: don't edit, regenerate (memenu sdk).\n\n";
			}

			static bool Copy(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error)
			{
				std::string text;
				if (!Read(from, text, error)) return false;
				Write(to, text);
				return true;
			}

			static bool Read(const std::filesystem::path& from, std::string& text, std::string& error)
			{
				std::ifstream in(from, std::ios::binary);
				if (!in) {
					error = "can't read " + from.string();
					return false;
				}
				std::stringstream s;
				s << in.rdbuf();
				text = s.str();
				text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
				return true;
			}

			static void Write(const std::filesystem::path& to, const std::string& text)
			{
				std::ofstream(to, std::ios::binary | std::ios::trunc) << text;
			}

			std::string Forward() const
			{
				std::string out = Banner() + "#pragma once\n// Every class, struct and enum of the SDK, declared\n\n"
											 "#include <cstdint>\n\nnamespace Classes\n{\n";
				for (const auto& [path, t] : m_.types)
					out += (t.isClass ? "\tclass " : "\tstruct ") + t.cpp + ";\n";
				for (const auto& [path, e] : m_.enums)
					if (Wanted(e.package)) out += "\tenum class " + e.cpp + " : std::uint8_t;\n";
				return out + "}\n";
			}

			bool Wanted(const std::string& package) const
			{
				return std::find(m_.packages.begin(), m_.packages.end(), package) != m_.packages.end();
			}

			std::vector<Member> MembersOf(const Type& t)
			{
				std::vector<Member> out;
				for (const Known& k : kKnown)
					if (t.path == k.path) out.push_back({k.name, k.cpp, "native", k.offset, k.size, 1, false, 0});
				for (const Property* p : m_.Members(t)) {
					Member mb;
					mb.name = Identifier(p->name);
					mb.offset = p->offset;
					mb.size = p->elementSize;
					mb.dim = p->arrayDim;
					mb.comment = Flags(p->flags);
					if (p->kind == Kind::Bool && p->arrayDim == 1) {
						mb.bit = true;
						mb.mask = p->boolMask;
						mb.cpp = "unsigned long";
					} else {
						mb.cpp = CppType(m_, *p, false);
						if (mb.cpp.empty()) {	// A type the SDK doesn't have: its bytes
							mb.cpp = "unsigned char";
							mb.dim *= mb.size;
							mb.size = 1;
							mb.comment += std::string(mb.comment.empty() ? "" : ", ") + "unknown type " + p->typePath;
						}
					}
					out.push_back(std::move(mb));
				}
				return out;
			}

			// Everything a class's members are called (up the supers), so a function doesn't hide one
			const std::set<std::string>& NamesOf(const Type& t)
			{
				if (auto it = names_.find(t.path); it != names_.end()) return it->second;
				std::set<std::string> names;
				if (const Type* super = t.base.empty() ? nullptr : m_.TypeAt(t.base)) names = NamesOf(*super);
				for (const Property& p : t.layout->properties) names.insert(Identifier(p.name));
				for (const Known& k : kKnown)
					if (t.path == k.path) names.insert(k.name);
				return names_.emplace(t.path, std::move(names)).first->second;
			}

			void Package(const std::string& pkg)
			{
				std::vector<const Type*> types;
				for (const auto& [path, t] : m_.types)
					if (t.package == pkg) types.push_back(&t);

				// In dependency order within the package: supers and structs held by value first
				std::vector<const Type*> order;
				std::set<const Type*> done, visiting;
				std::function<void(const Type*)> visit = [&](const Type* t) {
					if (done.count(t) || !visiting.insert(t).second) return;
					if (t->inherits)
						if (const Type* s = m_.TypeAt(t->base); s && s->package == pkg) visit(s);
					for (const Property* p : m_.Members(*t))
						if (p->kind == Kind::Struct)
							if (const Type* s = m_.TypeAt(p->typePath); s && s->package == pkg) visit(s);
					visiting.erase(t);
					done.insert(t);
					order.push_back(t);
				};
				for (const Type* t : types) visit(t);

				std::set<std::string> deps;
				for (const Type* t : types) {
					if (t->inherits) deps.insert(m_.TypeAt(t->base)->package);
					for (const Property* p : m_.Members(*t))
						if (p->kind == Kind::Struct)
							if (const Type* s = m_.TypeAt(p->typePath)) deps.insert(s->package);
				}
				deps.erase(pkg);

				std::string h = Banner() + "#pragma once\n\n#include \"sdk/Basic.hpp\"\n#include \"sdk/Forward.hpp\"\n";
				for (const std::string& d : deps) h += "#include \"sdk/" + d + ".hpp\"\n";
				h += "\n#pragma pack(push, 1)\n\nnamespace Classes\n{\n";
				std::string cpp = Banner() + "#include \"sdk/SDK.hpp\"\n\n#include <cstddef>\n#include <cstring>\n#include <string>\n#include <vector>\n\n"
											 "#define MIRAGE_OFFSET(T, m, o) static_assert(offsetof(T, m) == (o), #T \"::\" #m \" is not at \" #o)\n"
											 "#define MIRAGE_SIZE(T, s) static_assert(sizeof(T) == (s), #T \" is not \" #s \" bytes\")\n"
											 "#define MIRAGE_BIT(T, m, o, mask) failures += CheckBit<T>([](T& x) { x.m = 1; }, (o), (mask), #T \"::\" #m, first)\n\n"
											 "#pragma pack(push, 1)\n\nnamespace Classes\n{\n";
				Checks checks;

				// Enums
				for (const auto& [path, e] : m_.enums) {
					if (e.package != pkg) continue;
					h += fmt::format("\t// Enum {}\n\tenum class {} : std::uint8_t\n\t{{\n", path, e.cpp);
					for (std::size_t i = 0; i < e.values.size(); ++i)
						h += fmt::format("\t\t{} = {},\n", Identifier(e.values[i]), i);
					h += "\t};\n\n";
				}

				std::string params, functions;
				for (const Type* t : order) {
					const Type* super = t->base.empty() ? nullptr : m_.TypeAt(t->base);
					const std::uint32_t start = t->inherits ? super->layout->size : 0;
					const std::uint32_t end = t->isClass ? t->layout->size : t->elementSize;
					h += fmt::format("\t// {} {} (0x{:04X} bytes{})\n", t->isClass ? "Class" : "ScriptStruct", t->path,
									 end, t->fromEngine ? ", from the engine" : "");
					h += fmt::format("\t{} {}{}\n\t{{\n", t->isClass ? "class" : "struct", t->cpp,
									 t->inherits ? " : public " + super->cpp : std::string());
					if (t->isClass) h += "\tpublic:\n";
					h += Body(t->cpp, MembersOf(*t), start, end, true, checks, warnings_);
					// A native struct with no properties is 0 bytes to the engine; C++ makes it 1 (nothing holds one
					// by value, only arrays of pointers)
					if (end) checks.offsets.push_back(fmt::format("MIRAGE_SIZE({}, 0x{:X});", t->cpp, end));
					if (t->isClass) {
						++classes_;
						if (t->path == "Core.Object") h += "\n" + uobjectExtras_;
						h += "\n\t\tstatic class UClass* StaticClass();\n";
						functions += fmt::format("\tclass UClass* {}::StaticClass()\n\t{{\n\t\tstatic class UClass* c = nullptr;\n"
												 "\t\tif (!c) c = UObject::FindClass(\"Class {}\");\n\t\treturn c;\n\t}}\n\n",
												 t->cpp, t->path);
						std::vector<const Struct*> fns = t->functions;
						std::sort(fns.begin(), fns.end(), [](const Struct* a, const Struct* b) { return a->path < b->path; });
						for (const Struct* f : fns) Function(*t, *f, h, params, functions, checks);
					} else {
						++structs_;
					}
					h += "\t};\n\n";
				}
				h += "}\n\n#pragma pack(pop)\n";

				cpp += params;
				cpp += "\ttemplate <class T, class Set>\n\tint CheckBit(Set set, std::size_t offset, std::uint32_t mask, const char* what, std::string* first)\n"
					   "\t{\n\t\tstd::vector<unsigned char> buffer(sizeof(T), 0);\n\t\tset(*reinterpret_cast<T*>(buffer.data()));\n"
					   "\t\tstd::uint32_t v = 0;\n\t\tstd::memcpy(&v, buffer.data() + offset, sizeof v);\n"
					   "\t\tstd::size_t set_bytes = 0;\n\t\tfor (unsigned char b : buffer) set_bytes += b != 0;\n"
					   "\t\tif (v == mask && set_bytes == 1) return 0;\n\t\tif (first && first->empty()) *first = what;\n\t\treturn 1;\n\t}\n\n";
				cpp += functions;
				cpp += "\tnamespace detail\n\t{\n\t\tint CheckLayouts_" + pkg + "(std::string* first)\n\t\t{\n\t\t\tint failures = 0;\n\t\t\t(void)first;\n";
				for (const auto& b : checks.bits) cpp += "\t\t" + b + "\n";
				cpp += "\t\t\treturn failures;\n\t\t}\n\t}\n\n";
				for (const auto& o : checks.offsets) cpp += "\t" + o + "\n";
				cpp += "}\n\n#pragma pack(pop)\n";

				Write(o_.out / "include" / "sdk" / (pkg + ".hpp"), h);
				Write(o_.out / "src" / (pkg + ".cpp"), cpp);
			}

			void Function(const Type& t, const Struct& f, std::string& h, std::string& params, std::string& bodies,
						  Checks& checks)
			{
				const std::string fname = f.path.substr(f.path.find_last_of('.') + 1);
				std::string name = (f.functionFlags & kFuncStatic ? "STATIC_" : "") + Identifier(fname);
				if (NamesOf(t).count(name) || kHandWritten.count(name)) {
					warnings_.push_back(t.cpp + "::" + name + " renamed " + name + "_Fn: a member has its name");
					name += "_Fn";
				}

				std::vector<const Property*> parms;
				for (const Property& p : f.properties)
					if (p.flags & kParm) parms.push_back(&p);
				std::string ret = "void";
				const Property* retp = nullptr;
				std::vector<std::string> args;
				std::string in, out;
				for (const Property* p : parms) {
					const std::string id = Identifier(p->name);
					const std::string type = CppType(m_, *p, true);
					if (type.empty()) {
						++skipped_;
						warnings_.push_back(t.cpp + "::" + fname + " not generated: parameter " + p->name +
											" has a type the SDK doesn't have");
						return;
					}
					const bool isBool = p->kind == Kind::Bool;
					// A static array parameter (int KeyBinds[8]): a pointer to its elements, copied in (and back out)
					if (p->arrayDim > 1 && !(p->flags & kReturnParm)) {
						const bool isOut = (p->flags & kOutParm) != 0;
						args.push_back((isOut ? type + "* " : "const " + type + "* ") + id);
						const std::string set = isBool ? "p_.{0}[i] = {0}[i] ? 1u : 0u" : "p_.{0}[i] = {0}[i]";
						const std::string get = isBool ? "{0}[i] = p_.{0}[i] != 0" : "{0}[i] = p_.{0}[i]";
						in += fmt::format(fmt::runtime("\t\tif ({0}) for (int i = 0; i < {1}; ++i) " + set + ";\n"), id,
										  p->arrayDim);
						if (isOut)
							out += fmt::format(fmt::runtime("\t\tif ({0}) for (int i = 0; i < {1}; ++i) " + get + ";\n"),
											   id, p->arrayDim);
						continue;
					}
					if (p->flags & kReturnParm) {
						ret = type;
						retp = p;
						continue;
					}
					if (p->flags & kOutParm) {
						args.push_back(type + "* " + id);
						in += isBool ? fmt::format("\t\tif ({0}) p_.{0} = *{0} ? 1u : 0u;\n", id)
									 : fmt::format("\t\tif ({0}) p_.{0} = *{0};\n", id);
						out += isBool ? fmt::format("\t\tif ({0}) *{0} = p_.{0} != 0;\n", id)
									  : fmt::format("\t\tif ({0}) *{0} = p_.{0};\n", id);
						continue;
					}
					const bool byValue = p->kind == Kind::Byte || p->kind == Kind::Int || p->kind == Kind::Float ||
										 isBool || p->kind == Kind::Object || p->kind == Kind::Component ||
										 p->kind == Kind::Class || p->kind == Kind::Pointer;
					args.push_back((byValue ? type + " " : "const " + type + "& ") + id);
					in += isBool ? fmt::format("\t\tp_.{0} = {0} ? 1u : 0u;\n", id) : fmt::format("\t\tp_.{0} = {0};\n", id);
				}

				// The parameter struct, laid out as the engine laid the function out (UBOOL = 4 bytes)
				const std::string ps = t.cpp + "_" + fname + "_Params";
				std::vector<Member> members;
				std::uint32_t end = 0;
				for (const Property* p : parms) {
					Member mb;
					mb.name = Identifier(p->name);
					mb.cpp = p->kind == Kind::Bool ? "unsigned long" : CppType(m_, *p, false);
					mb.offset = p->offset;
					mb.size = p->elementSize;
					mb.dim = p->arrayDim;
					mb.comment = Flags(p->flags);
					end = std::max(end, p->offset + p->elementSize * p->arrayDim);
					members.push_back(std::move(mb));
				}
				params += fmt::format("\t// Function {} ({})\n\tstruct {}\n\t{{\n", f.path, FunctionFlagNames(f.functionFlags), ps);
				params += Body(ps, members, 0, end, false, checks, warnings_);
				params += "\t};\n\n";
				if (end) checks.offsets.push_back(fmt::format("MIRAGE_SIZE({}, 0x{:X});", ps, end));

				std::string list;
				for (std::size_t i = 0; i < args.size(); ++i) list += (i ? ", " : "") + args[i];
				h += fmt::format("\t\t{} {}({});\n", ret, name, list);
				bodies += fmt::format("\t// Function {} ({})\n\t{} {}::{}({})\n\t{{\n", f.path,
									  FunctionFlagNames(f.functionFlags), ret, t.cpp, name, list);
				bodies += fmt::format("\t\tstatic class UFunction* fn_ = nullptr;\n\t\tif (!fn_) fn_ = detail::FindFunction(\"{}\");\n", f.path);
				bodies += "\t\t" + ps + " p_{};\n" + in + "\t\tdetail::Call(this, fn_, &p_);\n" + out;
				if (retp) {
					const std::string id = Identifier(retp->name);
					bodies += retp->kind == Kind::Bool ? "\t\treturn p_." + id + " != 0;\n" : "\t\treturn p_." + id + ";\n";
				}
				bodies += "\t}\n\n";
				++functions_;
			}

			const Model& m_;
			const Options& o_;
			std::string uobjectExtras_;
			std::map<std::string, std::set<std::string>> names_;
			std::vector<std::string> warnings_;
			std::size_t classes_ = 0, structs_ = 0, functions_ = 0, skipped_ = 0;
		};
	}

	bool EmitCpp(const Model& m, const Options& o, Stats& stats, std::string& error)
	{
		Emitter e(m, o);
		if (!e.Run(error)) return false;
		stats.classes = e.Classes();
		stats.structs = e.Structs();
		stats.functions = e.Functions();
		stats.skippedFunctions = e.Skipped();
		stats.warnings = e.Warnings();
		return true;
	}
}
