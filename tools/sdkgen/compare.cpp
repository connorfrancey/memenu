// mirage_sdkgen: what the old (mmultiplayer) SDK has wrong. Its comments carry the offsets its dumper read, but code
// uses what the C++ compiles to, so the report works that out: the headers are parsed and laid out the way MSVC lays
// them out under the old SDK's #pragma pack(4) (bool 1 byte, unsigned long bitfields sharing 32-bit units, a derived
// class starting at its base's sizeof), then every member, bit and size is compared with the engine's layouts (the
// model: proven against the live engine by the self-test "Mirage layouts").

#include "emit.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <sstream>

namespace sdkgen
{
	namespace
	{
		struct OldMember
		{
			std::string type, name;
			std::uint32_t dim = 1;
			bool bit = false;
			std::uint32_t offset = 0, bitIndex = 0;	// Laid out
		};
		struct OldType
		{
			std::string path, kind;		// "Engine.Actor", "Class" / "ScriptStruct" / "Function"
			std::string cpp, base;
			std::vector<OldMember> members;
			// Laid out
			bool done = false, ok = true;
			std::string why;
			std::uint32_t size = 0, align = 1;
		};

		std::string Trim(std::string s)
		{
			const auto b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t\r");
			return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
		}

		bool ParseFile(const std::filesystem::path& file, std::map<std::string, OldType>& byCpp)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in) return false;
			static const std::regex head(R"(^(class|struct) (\w+)(?: : public (\w+))?\s*$)");
			static const std::regex comment(R"(^// (Class|ScriptStruct|Function) ([\w.]+))");
			static const std::regex member(
				R"(^\s*([A-Za-z_][\w:<>,* ]*?[\w>*])\s+(\w+)(?:\[(0x[0-9A-Fa-f]+|\d+)\])?(\s*:\s*1)?\s*;)");
			std::string line, lastPath, lastKind;
			OldType* cur = nullptr;
			int depth = 0;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				std::smatch m;
				if (!cur) {
					if (std::regex_search(line, m, comment)) {
						lastKind = m[1];
						lastPath = m[2];
					} else if (std::regex_match(line, m, head) && !lastPath.empty()) {
						OldType t;
						t.path = lastPath;
						t.kind = lastKind;
						t.cpp = m[2];
						t.base = m[3];
						cur = &(byCpp[t.cpp] = std::move(t));
						depth = 0;
						lastPath.clear();
					}
					continue;
				}
				for (char c : line) depth += c == '{' ? 1 : c == '}' ? -1 : 0;
				if (depth <= 0 && line.find("};") != std::string::npos) {
					cur = nullptr;
					continue;
				}
				if (depth != 1) continue;	// Inside a function body
				std::string code = line.substr(0, line.find("//"));
				if (code.find('(') != std::string::npos || code.find("static") != std::string::npos ||
					code.find("public:") != std::string::npos)
					continue;
				if (!std::regex_search(code, m, member)) continue;
				OldMember mb;
				mb.type = Trim(m[1]);
				mb.name = m[2];
				if (m[3].matched) mb.dim = static_cast<std::uint32_t>(std::stoul(m[3].str(), nullptr, 0));
				mb.bit = m[4].matched;
				cur->members.push_back(std::move(mb));
			}
			return true;
		}

		class OldLayout
		{
		public:
			explicit OldLayout(std::map<std::string, OldType>& types) : types_(types) {}

			// sizeof / alignof of a member type under pack(4); nullopt if unknown
			std::optional<std::pair<std::uint32_t, std::uint32_t>> SizeOf(std::string type)
			{
				if (!type.empty() && type.back() == '*') return std::pair{4u, 4u};
				for (const char* prefix : {"struct ", "class ", "enum class ", "enum "})
					if (type.rfind(prefix, 0) == 0) type.erase(0, std::strlen(prefix));
				type = Trim(type);
				static const std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> basic = {
					{"bool", {1, 1}}, {"char", {1, 1}}, {"unsigned char", {1, 1}}, {"uint8_t", {1, 1}},
					{"int8_t", {1, 1}}, {"short", {2, 2}}, {"unsigned short", {2, 2}}, {"int16_t", {2, 2}},
					{"uint16_t", {2, 2}}, {"int", {4, 4}}, {"unsigned int", {4, 4}}, {"long", {4, 4}},
					{"unsigned long", {4, 4}}, {"float", {4, 4}}, {"int32_t", {4, 4}}, {"uint32_t", {4, 4}},
					{"double", {8, 4}}, {"long long", {8, 4}}, {"unsigned long long", {8, 4}}, {"int64_t", {8, 4}},
					{"uint64_t", {8, 4}}, {"FName", {8, 4}}, {"FString", {12, 4}}, {"FScriptDelegate", {12, 4}},
					{"FScriptInterface", {8, 4}}, {"FPointer", {4, 4}}, {"FQWord", {8, 4}}};
				if (auto it = basic.find(type); it != basic.end()) return it->second;
				if (type.rfind("TArray<", 0) == 0) return std::pair{12u, 4u};
				if (type.rfind("TEnumAsByte<", 0) == 0) return std::pair{1u, 1u};
				if (type.rfind("TScriptInterface<", 0) == 0) return std::pair{8u, 4u};
				if (auto it = types_.find(type); it != types_.end()) {
					Lay(it->second);
					if (!it->second.ok) return std::nullopt;
					return std::pair{it->second.size, it->second.align};
				}
				return std::nullopt;
			}

			void Lay(OldType& t)
			{
				if (t.done) return;
				t.done = true;
				std::uint32_t cur = 0, align = 1;
				if (!t.base.empty()) {
					const auto base = types_.find(t.base);
					if (base == types_.end()) {
						t.ok = false;
						t.why = "base " + t.base + " not found";
						return;
					}
					Lay(base->second);
					if (!base->second.ok) {
						t.ok = false;
						t.why = "base " + t.base + ": " + base->second.why;
						return;
					}
					cur = base->second.size;
					align = base->second.align;
				}
				std::uint32_t unitAt = 0, bitsUsed = 32;
				bool prevBit = false;
				for (OldMember& m : t.members) {
					if (m.bit) {
						if (!prevBit || bitsUsed >= 32) {
							cur = (cur + 3) / 4 * 4;
							unitAt = cur;
							cur += 4;
							bitsUsed = 0;
						}
						m.offset = unitAt;
						m.bitIndex = bitsUsed++;
						align = std::max(align, 4u);
						prevBit = true;
						continue;
					}
					prevBit = false;
					const auto s = SizeOf(m.type);
					if (!s) {
						t.ok = false;
						t.why = m.name + ": unknown type " + m.type;
						return;
					}
					const std::uint32_t a = std::min(s->second, 4u);
					cur = (cur + a - 1) / a * a;
					m.offset = cur;
					cur += s->first * m.dim;
					align = std::max(align, a);
				}
				t.align = align;
				t.size = std::max(1u, (cur + align - 1) / align * align);
			}

		private:
			std::map<std::string, OldType>& types_;
		};

		int Log2(std::uint32_t v)
		{
			int n = 0;
			while (v > 1) { v >>= 1; ++n; }
			return n;
		}
	}

	bool CompareOldSdk(const Model& m, const Options& o, std::string& error)
	{
		std::map<std::string, OldType> old;
		std::size_t files = 0;
		for (const auto& entry : std::filesystem::directory_iterator(o.oldSdk)) {
			const std::string name = entry.path().filename().string();
			if (entry.path().extension() == ".hpp" && name.rfind("ME_", 0) == 0) files += ParseFile(entry.path(), old);
		}
		if (!files) {
			error = "no ME_*.hpp headers in " + o.oldSdk.string();
			return false;
		}
		OldLayout layout(old);
		for (auto& [cpp, t] : old) layout.Lay(t);

		struct Finding
		{
			std::string path, kind;
			std::vector<std::string> lines;
		};
		std::vector<Finding> findings;
		std::size_t compared = 0, members = 0, wrongMembers = 0, notLaid = 0, unmatched = 0;
		std::map<std::string, std::size_t> wrongByKind, comparedByKind;
		std::vector<std::string> notLaidOut;

		for (const auto& [cpp, t] : old) {
			const auto it = m.layouts.find(t.path);
			if (it == m.layouts.end()) {
				++unmatched;
				continue;
			}
			if (!t.ok) {
				++notLaid;
				notLaidOut.push_back(t.path + ": " + t.why);
				continue;
			}
			const Struct& s = it->second;
			++compared;
			++comparedByKind[t.kind];
			Finding f{t.path, t.kind, {}};

			std::map<std::string, const Property*> props;
			std::function<void(const Struct&)> collect = [&](const Struct& st) {
				for (const Property& p : st.properties) props.emplace(p.name, &p);
				if (!st.super.empty() && t.kind != "Function")
					if (auto sup = m.layouts.find(st.super); sup != m.layouts.end()) collect(sup->second);
			};
			collect(s);

			// The engine's size: a class's PropertySize, a struct's padded to its alignment (what a property holding
			// it takes, and what an array of it strides by), a function's parameters up to the last one
			std::uint32_t size = s.size;
			if (t.kind == "ScriptStruct")
				if (const Type* type = m.TypeAt(t.path)) size = type->elementSize;
			if (t.kind == "Function") {
				size = 0;
				for (const Property& p : s.properties)
					if (p.flags & 0x80) size = std::max(size, p.offset + p.elementSize * p.arrayDim);
			}
			if (t.size < size)
				f.lines.push_back(fmt::format("size 0x{:X}, engine 0x{:X}: too small", t.size, size));
			else if (t.kind != "Class" && (t.size + 3) / 4 * 4 != (size + 3) / 4 * 4)
				f.lines.push_back(fmt::format("size 0x{:X}, engine 0x{:X}", t.size, size));

			for (const OldMember& mb : t.members) {
				if (mb.name.rfind("UnknownData", 0) == 0) continue;
				std::string name = mb.name;
				auto p = props.find(name);
				if (p == props.end() && !name.empty() && name.back() == '_') p = props.find(name.substr(0, name.size() - 1));
				if (p == props.end()) continue;
				const Property& e = *p->second;
				++members;
				std::string what;
				if (mb.bit) {
					if (e.kind != Kind::Bool) what = "is a bit, the engine's is a " + std::string(KindName(e.kind));
					else if (mb.offset != e.offset || (e.boolMask && Log2(e.boolMask) != static_cast<int>(mb.bitIndex)))
						what = fmt::format("bit {} at 0x{:X}, engine bit {} at 0x{:X}", mb.bitIndex, mb.offset,
										   Log2(e.boolMask), e.offset);
				} else {
					if (mb.offset != e.offset)
						what = fmt::format("at 0x{:X}, engine 0x{:X}", mb.offset, e.offset);
					if (e.kind == Kind::Bool && mb.type == "bool")
						what += std::string(what.empty() ? "" : "; ") + "bool is 1 byte, the engine's UBOOL 4";
				}
				if (!what.empty()) {
					++wrongMembers;
					f.lines.push_back(mb.name + ": " + what);
				}
			}
			if (!f.lines.empty()) {
				++wrongByKind[t.kind];
				findings.push_back(std::move(f));
			}
		}

		std::sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
			return a.kind != b.kind ? a.kind < b.kind : a.path < b.path;
		});
		std::ostringstream r;
		r << "# What the old SDK has wrong\n\n"
		  << "Generated by mirage_sdkgen: the old (mmultiplayer) SDK's C++ layouts, as MSVC lays them out under its "
			 "`#pragma pack(4)`, against the engine's (proven by the in-game self-test \"Mirage layouts\").\n\n"
		  << "| | Compared | Wrong |\n|---|---:|---:|\n";
		for (const char* kind : {"Class", "ScriptStruct", "Function"})
			r << fmt::format("| {} | {} | {} |\n", std::string(kind) == "Function" ? "Parameter structs" : kind,
							 comparedByKind[kind], wrongByKind[kind]);
		r << fmt::format("\n{} types compared, {} with a mistake; {} members compared, {} wrong. {} old types aren't in "
						 "the generated SDK (Editor, UnrealEd, TdEditor), {} couldn't be laid out.\n",
						 compared, findings.size(), members, wrongMembers, unmatched, notLaid);
		std::string kind;
		for (const Finding& f : findings) {
			if (f.kind != kind) {
				kind = f.kind;
				r << "\n## " << (kind == "Function" ? "Parameter structs" : kind == "Class" ? "Classes" : "Structs")
				  << "\n";
			}
			r << "\n### " << f.path << "\n\n";
			for (const auto& l : f.lines) r << "- " << l << "\n";
		}
		if (!notLaidOut.empty()) {
			r << "\n## Not laid out\n\n";
			for (const auto& l : notLaidOut) r << "- " << l << "\n";
		}

		std::error_code ec;
		std::filesystem::create_directories(o.report.parent_path(), ec);
		std::ofstream out(o.report, std::ios::binary | std::ios::trunc);
		if (!out) {
			error = "can't write " + o.report.string();
			return false;
		}
		out << r.str();
		std::printf("old sdk   %zu types compared: %zu classes, %zu structs and %zu parameter structs wrong (%zu members)\n",
					compared, wrongByKind["Class"], wrongByKind["ScriptStruct"], wrongByKind["Function"], wrongMembers);
		return true;
	}
}
