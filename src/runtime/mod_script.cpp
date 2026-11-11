// Script patching for mods (Mirage, step 8): the MirageApi script_* functions over runtime/script_patch.

#include "runtime/mod_game.h"

#include "game/object_ref.h"
#include "runtime/mod_internal.h"
#include "runtime/script_patch.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

struct MirageScript
{
	MirageMod* mod = nullptr;
	std::string path;
	Classes::UStruct* function = nullptr;
	UnrealLib::Script script;
};

namespace mirage::mods
{
	namespace
	{
		using UnrealLib::Token;
		using UnrealLib::TokenKind;

		std::mutex g_lock;
		std::multimap<MirageMod*, std::unique_ptr<MirageScript>> g_open;	// Closed at the mod's unload at the latest

		// FName text, with the instance number as the engine prints it ("Name_2" for number 3)
		std::string NameText(const UnrealLib::FName& n)
		{
			if (n.index <= 0 && n.number == 0) return n.index == 0 ? "None" : "";
			Classes::FName f;
			f.Index = n.index;
			f.Number = n.number;
			std::string t = f.GetName();
			if (n.number > 0) t += "_" + std::to_string(n.number - 1);
			return t;
		}

		MirageObject* ObjOf(UnrealLib::ObjectIndex o)
		{
			return reinterpret_cast<MirageObject*>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(o.value)));
		}
		UnrealLib::ObjectIndex IndexOf(MirageObject* o)
		{
			return UnrealLib::ObjectIndex{static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(o))};
		}

		void ToToken(const MirageToken& in, Token& t)
		{
			t.kind = static_cast<TokenKind>(in.kind);
			t.native_index = static_cast<std::uint16_t>(in.native_index);
			t.int_value = in.int_value;
			std::memcpy(t.floats.data(), in.floats, sizeof in.floats);
			std::memcpy(t.ints.data(), in.ints, sizeof in.ints);
			t.object = IndexOf(in.object);
			t.object2 = IndexOf(in.object2);
			t.word = in.word;
			t.word2 = in.word2;
			t.byte_value = in.byte_value;
			t.byte_value2 = in.byte_value2;
			t.target = in.target;
			t.string_value.assign(in.text, strnlen(in.text, sizeof in.text));
		}

		bool ValidKind(int32_t kind)
		{
			return kind > 0 && kind <= static_cast<int32_t>(TokenKind::NativeFunction);
		}

		MirageScript* Live(MirageScript* s)
		{
			std::lock_guard lock(g_lock);
			for (const auto& [mod, open] : g_open)
				if (open.get() == s) return s;
			return nullptr;
		}
	}

	namespace game_api
	{
		MirageScript* MIRAGE_CALL ScriptOpen(MirageMod* mod, const char* function)
		{
			if (!mod || !function) return nullptr;
			auto* fn = scripts::Find(function);
			if (!fn) {
				spdlog::warn("[mod {}] script_open: no script function {}", mod->id, function);
				return nullptr;
			}
			auto s = std::make_unique<MirageScript>();
			s->mod = mod;
			s->path = function;
			s->function = fn;
			std::string error;
			if (!scripts::Read(fn, s->script, &error)) {
				spdlog::warn("[mod {}] script_open {}: the bytecode doesn't decode: {}", mod->id, function, error);
				return nullptr;
			}
			std::lock_guard lock(g_lock);
			MirageScript* handle = s.get();
			g_open.emplace(mod, std::move(s));
			return handle;
		}

		void MIRAGE_CALL ScriptClose(MirageScript* script)
		{
			std::lock_guard lock(g_lock);
			for (auto it = g_open.begin(); it != g_open.end(); ++it)
				if (it->second.get() == script) {
					g_open.erase(it);
					return;
				}
		}

		int32_t MIRAGE_CALL ScriptCount(MirageScript* script)
		{
			return Live(script) ? static_cast<int32_t>(script->script.tokens.size()) : 0;
		}

		int32_t MIRAGE_CALL ScriptGet(MirageScript* script, int32_t index, MirageToken* out)
		{
			if (!Live(script) || !out || out->size < sizeof(MirageToken) || index < 0 ||
				index >= static_cast<int32_t>(script->script.tokens.size()))
				return 0;
			const Token& t = script->script.tokens[static_cast<std::size_t>(index)];
			*out = MirageToken{};
			out->size = sizeof(MirageToken);
			out->kind = static_cast<int32_t>(t.kind);
			out->native_index = t.native_index;
			out->int_value = t.int_value;
			std::memcpy(out->floats, t.floats.data(), sizeof out->floats);
			std::memcpy(out->ints, t.ints.data(), sizeof out->ints);
			out->object = ObjOf(t.object);
			out->object2 = ObjOf(t.object2);
			const std::string name = t.kind == TokenKind::LabelTable ? std::string() : NameText(t.name);
			std::memcpy(out->name, name.data(), std::min(name.size(), sizeof out->name - 1));	// *out is zeroed
			out->word = t.word;
			out->word2 = t.word2;
			out->byte_value = t.byte_value;
			out->byte_value2 = t.byte_value2;
			out->target = t.target;
			out->memory_offset = t.memory_offset;
			std::memcpy(out->text, t.string_value.data(), std::min(t.string_value.size(), sizeof out->text - 1));
			return 1;
		}

		int32_t MIRAGE_CALL ScriptSet(MirageScript* script, int32_t index, const MirageToken* in)
		{
			if (!Live(script) || !in || in->size < sizeof(MirageToken) || !ValidKind(in->kind) || index < 0 ||
				index >= static_cast<int32_t>(script->script.tokens.size()))
				return 0;
			Token& t = script->script.tokens[static_cast<std::size_t>(index)];
			// The name: kept when unchanged (its instance number with it), else an existing name of the engine
			const std::string wanted(in->name, strnlen(in->name, sizeof in->name));
			UnrealLib::FName name = t.name;
			if (t.kind != TokenKind::LabelTable && wanted != NameText(t.name)) {
				const int i = wanted.empty() ? 0 : game::NameIndex(wanted.c_str());
				if (i < 0) {
					spdlog::warn("[mod {}] script_set: the engine has no name '{}'", script->mod->id, wanted);
					return 0;
				}
				name = {i, 0};
			}
			const auto children = t.children;
			const auto labels = t.labels;
			const auto memoryOffset = t.memory_offset, memorySize = t.memory_size;
			const auto opcode = t.opcode;
			ToToken(*in, t);
			t.name = name;
			t.children = children;		// Changed with script_set_children
			t.labels = labels;
			t.memory_offset = memoryOffset;	// Decoded tokens keep what assemble_script needs about where they were
			t.memory_size = memorySize;
			t.opcode = opcode;
			return 1;
		}

		int32_t MIRAGE_CALL ScriptAdd(MirageScript* script, const MirageToken* in)
		{
			if (!Live(script) || !in || in->size < sizeof(MirageToken) || !ValidKind(in->kind)) return -1;
			Token t;
			ToToken(*in, t);
			const std::string wanted(in->name, strnlen(in->name, sizeof in->name));
			if (!wanted.empty()) {
				const int i = game::NameIndex(wanted.c_str());
				if (i < 0) {
					spdlog::warn("[mod {}] script_add: the engine has no name '{}'", script->mod->id, wanted);
					return -1;
				}
				t.name = {i, 0};
			}
			script->script.tokens.push_back(std::move(t));		// memory_size 0: a new token
			return static_cast<int32_t>(script->script.tokens.size() - 1);
		}

		int32_t MIRAGE_CALL ScriptChildren(MirageScript* script, int32_t index, int32_t* out, int32_t capacity)
		{
			if (!Live(script)) return -1;
			const auto& tokens = script->script.tokens;
			if (index < -1 || index >= static_cast<int32_t>(tokens.size())) return -1;
			const auto& list = index < 0 ? script->script.statements : tokens[static_cast<std::size_t>(index)].children;
			for (int32_t i = 0; out && i < capacity && i < static_cast<int32_t>(list.size()); ++i)
				out[i] = static_cast<int32_t>(list[static_cast<std::size_t>(i)]);
			return static_cast<int32_t>(list.size());
		}

		int32_t MIRAGE_CALL ScriptSetChildren(MirageScript* script, int32_t index, const int32_t* children, int32_t count)
		{
			if (!Live(script) || count < 0 || (count > 0 && !children)) return 0;
			auto& tokens = script->script.tokens;
			if (index < -1 || index >= static_cast<int32_t>(tokens.size())) return 0;
			std::vector<std::uint32_t> list;
			for (int32_t i = 0; i < count; ++i) {
				if (children[i] < 0 || children[i] >= static_cast<int32_t>(tokens.size()) || children[i] == index) return 0;
				list.push_back(static_cast<std::uint32_t>(children[i]));
			}
			(index < 0 ? script->script.statements : tokens[static_cast<std::size_t>(index)].children) = std::move(list);
			return 1;
		}

		int32_t MIRAGE_CALL ScriptApply(MirageScript* script)
		{
			if (!Live(script)) return 0;
			std::string error;
			if (!scripts::Apply(script->function, script->script, script->mod->id, &error)) {
				spdlog::warn("[mod {}] script_apply {}: {}", script->mod->id, script->path, error);
				return 0;
			}
			return 1;
		}

		int32_t MIRAGE_CALL ScriptRestore(MirageMod* mod, const char* function)
		{
			if (!mod || !function) return 0;
			auto* fn = scripts::Find(function);
			return fn && scripts::Restore(fn) ? 1 : 0;
		}

		uint32_t MIRAGE_CALL ScriptDump(MirageScript* script, char* buffer, uint32_t capacity)
		{
			const std::string text = Live(script) ? scripts::Dump(script->script) : std::string();
			if (buffer && capacity) {
				const std::size_t n = std::min<std::size_t>(text.size(), capacity - 1);
				std::memcpy(buffer, text.data(), n);
				buffer[n] = '\0';
			}
			return static_cast<uint32_t>(text.size());
		}

		int32_t MIRAGE_CALL ScriptKind(const char* name)
		{
			if (!name) return -1;
			const TokenKind k = UnrealLib::parse_token_kind(name);
			return k == TokenKind::Bad ? -1 : static_cast<int32_t>(k);
		}

		const char* MIRAGE_CALL ScriptKindName(int32_t kind)
		{
			static std::map<int32_t, std::string> names;	// Stable pointers for the C side
			static std::mutex lock;
			std::lock_guard l(lock);
			auto [it, added] = names.try_emplace(kind);
			if (added) it->second = std::string(UnrealLib::to_string(static_cast<TokenKind>(kind)));
			return it->second.c_str();
		}
	}

	void detail::ReleaseScripts(MirageMod& mod)
	{
		scripts::RestoreOwner(mod.id);
		std::lock_guard lock(g_lock);
		g_open.erase(&mod);
	}
}
