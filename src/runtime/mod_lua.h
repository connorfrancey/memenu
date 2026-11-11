#pragma once
// Lua mods (Mirage step 4): main.lua runs in its own sandboxed state (runtime/lua_vm.h), with a `mirage` table bound
// to the same MirageApi table native mods get, and to nothing else, so the C ABI and Lua can't drift apart.
//
//     local m = mirage.module{ name = "Hello (Lua)", tab = "Mods", description = "Says hello",
//                              on_render = function() mirage.draw.text(20, 240, mirage.rgba(255, 255, 255), greeting:get()) end }
//     greeting = m:text("Greeting", "Hello from Lua")
//     size = m:slider("Size", 1.0, 0.5, 3.0)
//
// mirage.log / warn / error, mirage.id / version / dir, mirage.module{...} (on_enable, on_disable, on_tick, on_render,
// on_trigger, action, enabled_by_default, key, order), module:checkbox / slider / slider_int / text / key,
// setting:get / set / down, mirage.setting("key") (a setting declared in mod.toml), mirage.config.get / set,
// mirage.draw.text / line / rect / rect_filled / screen_size / text_size, mirage.rgba, and require("name") for name.lua
// in the mod's folder.

#include "mirage/api.h"

#include <filesystem>
#include <memory>
#include <string>

namespace mirage::mods
{
	class LuaMod;
	struct LuaModDeleter
	{
		void operator()(LuaMod* m) const;
	};
	using LuaModPtr = std::unique_ptr<LuaMod, LuaModDeleter>;

	// Runs `file` (the mod's main.lua) for `mod`. nullptr and `error` when it doesn't load or run.
	LuaModPtr LoadLua(const MirageApi* api, MirageMod* mod, const std::filesystem::path& file, std::string& error);
	// Lua's memory use (bytes) of a loaded mod
	std::size_t LuaMemory(const LuaMod& m);
}
