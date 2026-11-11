#include "runtime/script_patch.h"

#include "game/object_cache.h"
#include "sdk/SDK.hpp"

#include <UnrealLib/Core/Error.hpp>
#include <UnrealLib/Package/GameProfile.hpp>
#include <UnrealLib/Serialization/BinaryReader.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>

#include <Windows.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <climits>
#include <cstring>
#include <map>
#include <span>

namespace mirage::scripts
{
	using namespace UnrealLib;

	namespace
	{
		constexpr std::uint16_t kFileVersion = 536;		// Mirror's Edge: object operands 4 bytes, names 8, in memory too

		// UStruct::Script, a TArray<BYTE> (its members are private in the SDK)
		struct RawArray
		{
			std::uint8_t* data;
			std::int32_t count, max;
		};
		static_assert(sizeof(RawArray) == sizeof(Classes::TArray<unsigned char>));
		RawArray& ScriptOf(Classes::UStruct* s) { return *reinterpret_cast<RawArray*>(&s->Script); }

		struct Record
		{
			RawArray original;
			std::string owner;
			std::size_t patches = 0;
		};
		std::map<Classes::UStruct*, Record> g_patched;

		SerializationContext Context()
		{
			SerializationContext ctx;
			ctx.file_version = kFileVersion;
			return ctx;
		}
	}

	const TokenMap& Map()
	{
		static const TokenMap map = [] {
			const GameProfile* profile = GameProfileRegistry::builtin().find("mirrorsedge-pc");
			return TokenMap::ue3(kFileVersion, profile);
		}();
		return map;
	}

	Classes::UStruct* Find(const std::string& path)
	{
		for (const char* kind : {"Function ", "State "}) {
			if (auto* s = game::FindObject<Classes::UStruct>(std::string(kind) + path); s && ScriptOf(s).count > 0) return s;
		}
		return nullptr;
	}

	bool Read(Classes::UStruct* function, Script& out, std::string* error)
	{
		const RawArray& code = ScriptOf(function);
		if (!code.data || code.count <= 0) {
			if (error) *error = "it has no script";
			return false;
		}
		try {
			const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(code.data), static_cast<std::size_t>(code.count));
			BinaryReader reader(bytes, Context());
			BytecodeOptions options;
			options.token_map = &Map();
			options.name_count = INT32_MAX;		// Global FNames, not a package's
			options.none_index = 0;				// NAME_None
			out = decode_script(reader, static_cast<std::size_t>(code.count), options);
			return true;
		} catch (const std::exception& e) {
			if (error) *error = e.what();
			return false;
		}
	}

	bool Apply(Classes::UStruct* function, const Script& script, const std::string& owner, std::string* error)
	{
		std::vector<std::byte> bytes;
		try {
			BinaryWriter writer(Context());
			const std::size_t memory = assemble_script(writer, script, Map());
			bytes.assign(writer.data().begin(), writer.data().end());
			if (memory != bytes.size()) throw std::runtime_error("memory and file sizes differ");
		} catch (const std::exception& e) {
			if (error) *error = e.what();
			return false;
		}
		// Outside this DLL's heap, and never freed: a call may still run in it (Restore keeps it too)
		auto* buffer = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), 0, bytes.size()));
		if (!buffer) {
			if (error) *error = "out of memory";
			return false;
		}
		std::memcpy(buffer, bytes.data(), bytes.size());
		RawArray& code = ScriptOf(function);
		auto [it, first] = g_patched.try_emplace(function, Record{code, owner, 0});
		if (!first && it->second.owner != owner)
			spdlog::warn("Scripts: {} patched by {} over {}'s patch", function->GetPathName(), owner, it->second.owner);
		it->second.owner = owner;
		++it->second.patches;
		code.data = buffer;
		code.count = code.max = static_cast<std::int32_t>(bytes.size());
		spdlog::info("Scripts: {} patched by {}: {} bytes (was {})", function->GetPathName(), owner, bytes.size(),
					 it->second.original.count);
		return true;
	}

	bool Restore(Classes::UStruct* function)
	{
		const auto it = g_patched.find(function);
		if (it == g_patched.end()) return false;
		ScriptOf(function) = it->second.original;
		spdlog::info("Scripts: {} back to its own code", function->GetPathName());
		g_patched.erase(it);
		return true;
	}

	void RestoreOwner(const std::string& owner)
	{
		for (auto it = g_patched.begin(); it != g_patched.end();) {
			const auto next = std::next(it);
			if (it->second.owner == owner) Restore(it->first);
			it = next;
		}
	}

	void RestoreAll()
	{
		while (!g_patched.empty()) Restore(g_patched.begin()->first);
	}

	bool Patched(Classes::UStruct* function) { return g_patched.count(function) != 0; }

	std::string Dump(const Script& script)
	{
		std::string out;
		const auto line = [&](auto&& self, std::uint32_t index, int depth) -> void {
			if (index >= script.tokens.size() || depth > 64) return;
			const Token& t = script.tokens[index];
			std::string ops;
			switch (t.kind) {
				case TokenKind::NativeFunction: ops = fmt::format(" native {}", t.native_index); break;
				case TokenKind::IntConst: ops = fmt::format(" {}", t.int_value); break;
				case TokenKind::FloatConst: ops = fmt::format(" {}", t.floats[0]); break;
				case TokenKind::ByteConst: case TokenKind::IntConstByte: ops = fmt::format(" {}", t.byte_value); break;
				case TokenKind::StringConst: case TokenKind::UnicodeStringConst: ops = " \"" + t.string_value + "\""; break;
				case TokenKind::Jump: case TokenKind::JumpIfNot: case TokenKind::Iterator: ops = fmt::format(" -> 0x{:X}", t.word); break;
				default: break;
			}
			if (!t.object.is_null()) {
				auto* o = reinterpret_cast<Classes::UObject*>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(t.object.value)));
				ops += " " + (o ? o->GetName() : std::string("none"));
			}
			if (t.name.index > 0 && t.kind != TokenKind::LabelTable) {
				Classes::FName n;
				n.Index = t.name.index;
				n.Number = t.name.number;
				ops += " '" + n.GetName() + "'";
			}
			out += fmt::format("#{:<4} {:04X} {}{}{}\n", index, t.memory_offset, std::string(static_cast<std::size_t>(depth) * 2, ' '),
							   to_string(t.kind), ops);
			for (const auto c : t.children) self(self, c, depth + 1);
		};
		for (const auto s : script.statements) line(line, s, 0);
		return out;
	}
}
