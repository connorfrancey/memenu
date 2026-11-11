// Lua mods: the `mirage` table, bound to MirageApi only (runtime/mod_lua.h).
//
// One state per mod. Callbacks come from the game thread (on_tick) and the render thread (on_render), so every entry
// into the state holds the mod's lock. Each call gets the state's instruction budget; a callback that errors is
// logged with the mod's name and its module switched off (it would otherwise log the same error every frame).

#include "runtime/mod_lua.h"

#include "runtime/lua_vm.h"

#include <lauxlib.h>
#include <lua.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace mirage::mods
{
	namespace
	{
		constexpr const char* kModuleMeta = "mirage.module";
		constexpr const char* kSettingMeta = "mirage.setting";

		enum class SettingKind : std::uint8_t { Checkbox, Slider, SliderInt, Text, Key, Choice };
		struct SettingBox
		{
			MirageSetting* handle;
			SettingKind kind;
		};
		enum Hook { kEnable, kDisable, kTick, kRender, kTrigger, kHooks };
		const char* const kHookNames[kHooks] = {"on_enable", "on_disable", "on_tick", "on_render", "on_trigger"};
	}

	class LuaMod
	{
	public:
		struct ModuleRefs		// A module's Lua callbacks: the `user` of its MirageModuleDesc
		{
			LuaMod* owner = nullptr;
			MirageModule* handle = nullptr;
			std::string name;
			int refs[kHooks] = {LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};
			bool failed = false;
		};

		LuaMod(const MirageApi* api, MirageMod* mod) : api_(api), mod_(mod), state_(32u << 20, 20'000'000) {}

		bool Load(const std::filesystem::path& file, std::string& error);
		std::size_t Memory() const { return state_.MemoryUsed(); }

	private:
		static LuaMod& Self(lua_State* L) { return *static_cast<LuaMod*>(lua_touserdata(L, lua_upvalueindex(1))); }
		void Bind();
		void Log(int level, const std::string& text) { api_->log(mod_, level, text.c_str()); }

		// ---- Callbacks from the runtime ----------------------------------------------------------------------------
		template <int H>
		static void MIRAGE_CALL Invoke(void* user)
		{
			auto* refs = static_cast<ModuleRefs*>(user);
			refs->owner->Run(*refs, H);
		}
		void Run(ModuleRefs& refs, int hook)
		{
			std::lock_guard lock(lock_);
			if (refs.failed || refs.refs[hook] == LUA_NOREF) return;
			lua_State* L = state_.Get();
			lua_rawgeti(L, LUA_REGISTRYINDEX, refs.refs[hook]);
			const lua::Result r = state_.Call(0, 0);
			if (r.ok) return;
			refs.failed = true;
			Log(MIRAGE_LOG_ERROR, refs.name + " " + kHookNames[hook] + ": " + r.error + " (the module is stopped)");
			if (hook != kDisable && refs.handle && api_->module_enabled(refs.handle)) api_->module_set_enabled(refs.handle, 0);
		}

		// ---- The `mirage` table -----------------------------------------------------------------------------------
		static std::string Concat(lua_State* L)
		{
			std::string text;
			for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
				if (i > 1) text += ' ';
				std::size_t len = 0;
				const char* s = luaL_tolstring(L, i, &len);
				text.append(s, len);
				lua_pop(L, 1);
			}
			return text;
		}
		template <int Level>
		static int LuaLog(lua_State* L)
		{
			Self(L).Log(Level, Concat(L));
			return 0;
		}

		static int LuaModule(lua_State* L);
		static int ModuleEnabled(lua_State* L);
		static int ModuleSetEnabled(lua_State* L);
		static int ModuleCheckbox(lua_State* L);
		static int ModuleSlider(lua_State* L);
		static int ModuleSliderInt(lua_State* L);
		static int ModuleText(lua_State* L);
		static int ModuleKey(lua_State* L);
		static int FindSetting(lua_State* L);
		static int SettingGet(lua_State* L);
		static int SettingSet(lua_State* L);
		static int SettingDown(lua_State* L);
		static int ConfigGet(lua_State* L);
		static int ConfigSet(lua_State* L);
		static int DrawText(lua_State* L);
		static int DrawLine(lua_State* L);
		static int DrawRect(lua_State* L);
		static int DrawRectFilled(lua_State* L);
		static int ScreenSize(lua_State* L);
		static int TextSize(lua_State* L);
		static int Rgba(lua_State* L);
		static int Require(lua_State* L);

		static MirageModule* CheckModule(lua_State* L)
		{
			auto** refs = static_cast<ModuleRefs**>(luaL_checkudata(L, 1, kModuleMeta));
			if (!(*refs)->handle) luaL_error(L, "the module %s wasn't registered", (*refs)->name.c_str());
			return (*refs)->handle;
		}
		static int PushSetting(lua_State* L, MirageSetting* handle, SettingKind kind)
		{
			if (!handle) {
				lua_pushnil(L);
				return 1;
			}
			auto* box = static_cast<SettingBox*>(lua_newuserdatauv(L, sizeof(SettingBox), 0));
			*box = {handle, kind};
			luaL_setmetatable(L, kSettingMeta);
			return 1;
		}
		static const char* OptString(lua_State* L, int index) { return luaL_optstring(L, index, ""); }

		const MirageApi* api_;
		MirageMod* mod_;
		lua::State state_;
		std::recursive_mutex lock_;		// Recursive: a callback may switch its module off, which calls on_disable
		std::vector<std::unique_ptr<ModuleRefs>> modules_;
		std::filesystem::path dir_;
	};

	void LuaModDeleter::operator()(LuaMod* m) const { delete m; }

	std::size_t LuaMemory(const LuaMod& m) { return m.Memory(); }

	// ---- mirage.module{...} ----------------------------------------------------------------------------------------
	int LuaMod::LuaModule(lua_State* L)
	{
		LuaMod& self = Self(L);
		luaL_checktype(L, 1, LUA_TTABLE);
		const auto field = [&](const char* key) { return lua_getfield(L, 1, key); };	// Pushes it
		const auto text = [&](const char* key) {
			std::string s;
			if (field(key) == LUA_TSTRING) s = lua_tostring(L, -1);
			lua_pop(L, 1);
			return s;
		};
		const auto integer = [&](const char* key) {
			lua_Integer v = 0;
			if (field(key) == LUA_TNUMBER) v = lua_tointeger(L, -1);
			lua_pop(L, 1);
			return static_cast<int32_t>(v);
		};
		const auto flag = [&](const char* key) {
			field(key);
			const bool v = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return v;
		};

		auto refs = std::make_unique<ModuleRefs>();
		refs->owner = &self;
		refs->name = text("name");
		if (refs->name.empty()) return luaL_error(L, "mirage.module: name is required");
		for (int h = 0; h < kHooks; ++h) {
			const int type = field(kHookNames[h]);
			if (type == LUA_TFUNCTION) refs->refs[h] = luaL_ref(L, LUA_REGISTRYINDEX);	// Pops it
			else if (type == LUA_TNIL) lua_pop(L, 1);
			else return luaL_error(L, "mirage.module: %s must be a function", kHookNames[h]);
		}
		const std::string tab = text("tab"), description = text("description");

		MirageModuleDesc d{};
		d.size = sizeof d;
		d.name = refs->name.c_str();
		d.tab = tab.empty() ? nullptr : tab.c_str();
		d.description = description.c_str();
		d.order = integer("order");
		d.key = integer("key");
		d.flags = (flag("action") ? MIRAGE_MODULE_ACTION : 0u) | (flag("enabled_by_default") ? MIRAGE_MODULE_ENABLED_BY_DEFAULT : 0u);
		d.user = refs.get();
		d.on_enable = &Invoke<kEnable>;
		d.on_disable = &Invoke<kDisable>;
		d.on_tick = &Invoke<kTick>;
		d.on_render = &Invoke<kRender>;
		d.on_trigger = &Invoke<kTrigger>;
		refs->handle = self.api_->module_register(self.mod_, &d);
		if (!refs->handle) {
			for (int ref : refs->refs) luaL_unref(L, LUA_REGISTRYINDEX, ref);
			return luaL_error(L, "mirage.module: '%s' can't be registered (is the name taken?)", refs->name.c_str());
		}
		auto** box = static_cast<ModuleRefs**>(lua_newuserdatauv(L, sizeof(ModuleRefs*), 0));
		*box = refs.get();
		luaL_setmetatable(L, kModuleMeta);
		self.modules_.push_back(std::move(refs));
		return 1;
	}

	int LuaMod::ModuleEnabled(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->module_enabled(CheckModule(L)));
		return 1;
	}
	int LuaMod::ModuleSetEnabled(lua_State* L)
	{
		Self(L).api_->module_set_enabled(CheckModule(L), lua_toboolean(L, 2));
		return 0;
	}
	int LuaMod::ModuleCheckbox(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		return PushSetting(L, Self(L).api_->setting_checkbox(m, luaL_checkstring(L, 2), lua_toboolean(L, 3), OptString(L, 4)),
						   SettingKind::Checkbox);
	}
	int LuaMod::ModuleSlider(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto value = static_cast<float>(luaL_checknumber(L, 3));
		const auto min = static_cast<float>(luaL_checknumber(L, 4)), max = static_cast<float>(luaL_checknumber(L, 5));
		return PushSetting(L, Self(L).api_->setting_slider(m, luaL_checkstring(L, 2), value, min, max,
															luaL_optstring(L, 6, "%.2f"), OptString(L, 7)),
						   SettingKind::Slider);
	}
	int LuaMod::ModuleSliderInt(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto value = static_cast<int32_t>(luaL_checkinteger(L, 3));
		const auto min = static_cast<int32_t>(luaL_checkinteger(L, 4)), max = static_cast<int32_t>(luaL_checkinteger(L, 5));
		return PushSetting(L, Self(L).api_->setting_slider_int(m, luaL_checkstring(L, 2), value, min, max, OptString(L, 6)),
						   SettingKind::SliderInt);
	}
	int LuaMod::ModuleText(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		return PushSetting(L, Self(L).api_->setting_text(m, luaL_checkstring(L, 2), OptString(L, 3), OptString(L, 4)),
						   SettingKind::Text);
	}
	int LuaMod::ModuleKey(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto vk = static_cast<int32_t>(luaL_optinteger(L, 3, 0));
		return PushSetting(L, Self(L).api_->setting_key(m, luaL_checkstring(L, 2), vk, OptString(L, 4)), SettingKind::Key);
	}

	// mirage.setting("key"): a setting declared in mod.toml, nil when there is none
	int LuaMod::FindSetting(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageSetting* h = self.api_->setting_find(self.mod_, luaL_checkstring(L, 1));
		if (!h) {
			lua_pushnil(L);
			return 1;
		}
		SettingKind kind = SettingKind::Text;
		switch (self.api_->setting_type(h)) {
			case MIRAGE_SETTING_BOOL: kind = SettingKind::Checkbox; break;
			case MIRAGE_SETTING_INT: kind = SettingKind::SliderInt; break;
			case MIRAGE_SETTING_FLOAT: kind = SettingKind::Slider; break;
			case MIRAGE_SETTING_CHOICE: kind = SettingKind::Choice; break;
			case MIRAGE_SETTING_KEY: kind = SettingKind::Key; break;
			default: break;
		}
		return PushSetting(L, h, kind);
	}

	int LuaMod::SettingGet(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		const MirageApi* api = Self(L).api_;
		switch (box->kind) {
			case SettingKind::Checkbox: lua_pushboolean(L, api->setting_get_int(box->handle)); break;
			case SettingKind::Slider: lua_pushnumber(L, api->setting_get_float(box->handle)); break;
			case SettingKind::SliderInt: case SettingKind::Key: lua_pushinteger(L, api->setting_get_int(box->handle)); break;
			case SettingKind::Text: case SettingKind::Choice: {
				const uint32_t n = api->setting_get_text(box->handle, nullptr, 0);
				std::string s(n + 1, '\0');
				api->setting_get_text(box->handle, s.data(), n + 1);
				lua_pushlstring(L, s.data(), n);
				break;
			}
		}
		return 1;
	}
	int LuaMod::SettingSet(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		const MirageApi* api = Self(L).api_;
		switch (box->kind) {
			case SettingKind::Checkbox: api->setting_set_int(box->handle, lua_toboolean(L, 2)); break;
			case SettingKind::Slider: api->setting_set_float(box->handle, static_cast<float>(luaL_checknumber(L, 2))); break;
			case SettingKind::SliderInt: api->setting_set_int(box->handle, static_cast<int32_t>(luaL_checkinteger(L, 2))); break;
			case SettingKind::Text: api->setting_set_text(box->handle, luaL_checkstring(L, 2)); break;
			case SettingKind::Key: return luaL_error(L, "a key setting is set by binding it in the menu");
			case SettingKind::Choice:
				if (lua_type(L, 2) == LUA_TNUMBER) api->setting_set_int(box->handle, static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1);
				else api->setting_set_text(box->handle, luaL_checkstring(L, 2));
				break;
		}
		return 0;
	}
	int LuaMod::SettingDown(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		lua_pushboolean(L, box->kind == SettingKind::Key && Self(L).api_->key_down(box->handle));
		return 1;
	}

	int LuaMod::ConfigGet(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* key = luaL_checkstring(L, 1);
		const bool hasFallback = !lua_isnoneornil(L, 2);
		const std::string fallback = hasFallback ? luaL_tolstring(L, 2, nullptr) : "";
		if (hasFallback) lua_pop(L, 1);
		const uint32_t n = self.api_->config_get(self.mod_, key, fallback.c_str(), nullptr, 0);
		std::string s(n + 1, '\0');
		self.api_->config_get(self.mod_, key, fallback.c_str(), s.data(), n + 1);
		if (n == 0 && !hasFallback) lua_pushnil(L);
		else lua_pushlstring(L, s.data(), n);
		return 1;
	}
	int LuaMod::ConfigSet(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* key = luaL_checkstring(L, 1);
		const std::string value = luaL_tolstring(L, 2, nullptr);
		lua_pop(L, 1);
		self.api_->config_set(self.mod_, key, value.c_str());
		return 0;
	}

	namespace
	{
		uint32_t Color(lua_State* L, int index)
		{
			return static_cast<uint32_t>(luaL_optinteger(L, index, 0xFFFFFFFF));
		}
		float Num(lua_State* L, int index) { return static_cast<float>(luaL_checknumber(L, index)); }
	}

	int LuaMod::DrawText(lua_State* L)
	{
		Self(L).api_->draw_text(Num(L, 1), Num(L, 2), Color(L, 3), luaL_checkstring(L, 4));
		return 0;
	}
	int LuaMod::DrawLine(lua_State* L)
	{
		Self(L).api_->draw_line(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5),
								static_cast<float>(luaL_optnumber(L, 6, 1.0)));
		return 0;
	}
	int LuaMod::DrawRect(lua_State* L)
	{
		Self(L).api_->draw_rect(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5),
								static_cast<float>(luaL_optnumber(L, 6, 1.0)));
		return 0;
	}
	int LuaMod::DrawRectFilled(lua_State* L)
	{
		Self(L).api_->draw_rect_filled(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5));
		return 0;
	}
	int LuaMod::ScreenSize(lua_State* L)
	{
		float w = 0, h = 0;
		Self(L).api_->screen_size(&w, &h);
		lua_pushnumber(L, w);
		lua_pushnumber(L, h);
		return 2;
	}
	int LuaMod::TextSize(lua_State* L)
	{
		float w = 0, h = 0;
		Self(L).api_->text_size(luaL_checkstring(L, 1), &w, &h);
		lua_pushnumber(L, w);
		lua_pushnumber(L, h);
		return 2;
	}
	int LuaMod::Rgba(lua_State* L)
	{
		const auto part = [&](int i, lua_Integer fallback) {
			const lua_Integer v = luaL_optinteger(L, i, fallback);
			return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v);
		};
		lua_pushinteger(L, static_cast<lua_Integer>((part(4, 255) << 24) | (part(3, 0) << 16) | (part(2, 0) << 8) | part(1, 0)));
		return 1;
	}

	// require("name"): name.lua (dots for folders) in the mod's folder, run once, its result kept
	int LuaMod::Require(lua_State* L)
	{
		LuaMod& self = Self(L);
		std::string name = luaL_checkstring(L, 1);
		for (char c : name)
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
				return luaL_error(L, "require: '%s' is not a module name", name.c_str());
		lua_getfield(L, LUA_REGISTRYINDEX, "mirage.loaded");
		if (lua_getfield(L, -1, name.c_str()) != LUA_TNIL) return 1;
		lua_pop(L, 1);
		std::string rel = name;
		for (char& c : rel)
			if (c == '.') c = '/';
		const auto file = self.dir_ / std::filesystem::path(std::u8string(rel.begin(), rel.end()) + u8".lua");
		std::ifstream in(file, std::ios::binary);
		if (!in) return luaL_error(L, "require: %s.lua is not in the mod's folder", rel.c_str());
		std::stringstream text;
		text << in.rdbuf();
		const std::string code = text.str(), chunk = "=" + rel + ".lua";
		if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
		lua_call(L, 0, 1);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_pushboolean(L, 1);
		}
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, name.c_str());	// mirage.loaded[name]
		return 1;
	}

	void LuaMod::Bind()
	{
		lua_State* L = state_.Get();
		const auto set = [&](const luaL_Reg* fns) {	// Into the table on top, each with `this` as upvalue
			lua_pushlightuserdata(L, this);
			luaL_setfuncs(L, fns, 1);
		};

		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "mirage.loaded");

		luaL_newmetatable(L, kModuleMeta);
		lua_newtable(L);
		const luaL_Reg moduleFns[] = {{"enabled", ModuleEnabled}, {"set_enabled", ModuleSetEnabled},
			{"checkbox", ModuleCheckbox}, {"slider", ModuleSlider}, {"slider_int", ModuleSliderInt}, {"text", ModuleText},
			{"key", ModuleKey}, {nullptr, nullptr}};
		set(moduleFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		luaL_newmetatable(L, kSettingMeta);
		lua_newtable(L);
		const luaL_Reg settingFns[] = {{"get", SettingGet}, {"set", SettingSet}, {"down", SettingDown}, {nullptr, nullptr}};
		set(settingFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		lua_newtable(L);	// mirage
		const luaL_Reg fns[] = {{"log", LuaLog<MIRAGE_LOG_INFO>}, {"info", LuaLog<MIRAGE_LOG_INFO>},
			{"warn", LuaLog<MIRAGE_LOG_WARN>}, {"error", LuaLog<MIRAGE_LOG_ERROR>}, {"module", LuaModule},
			{"setting", FindSetting}, {"rgba", Rgba}, {nullptr, nullptr}};
		set(fns);
		lua_pushstring(L, api_->mod_id(mod_));
		lua_setfield(L, -2, "id");
		lua_pushstring(L, api_->mod_version(mod_));
		lua_setfield(L, -2, "version");
		lua_pushstring(L, api_->mod_dir(mod_));
		lua_setfield(L, -2, "dir");
		lua_pushstring(L, api_->runtime_version);
		lua_setfield(L, -2, "runtime_version");

		lua_newtable(L);
		const luaL_Reg configFns[] = {{"get", ConfigGet}, {"set", ConfigSet}, {nullptr, nullptr}};
		set(configFns);
		lua_setfield(L, -2, "config");

		lua_newtable(L);
		const luaL_Reg drawFns[] = {{"text", DrawText}, {"line", DrawLine}, {"rect", DrawRect},
			{"rect_filled", DrawRectFilled}, {"screen_size", ScreenSize}, {"text_size", TextSize}, {nullptr, nullptr}};
		set(drawFns);
		lua_setfield(L, -2, "draw");
		lua_setglobal(L, "mirage");

		lua_pushlightuserdata(L, this);
		lua_pushcclosure(L, Require, 1);
		lua_setglobal(L, "require");
	}

	bool LuaMod::Load(const std::filesystem::path& file, std::string& error)
	{
		if (!state_.Valid()) {
			error = "no Lua state (out of memory)";
			return false;
		}
		dir_ = file.parent_path();
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			error = "can't be read";
			return false;
		}
		std::stringstream text;
		text << in.rdbuf();
		std::lock_guard lock(lock_);
		Bind();
		const auto name = file.filename().u8string();
		const lua::Result r = state_.Run(text.str(), std::string(name.begin(), name.end()));
		if (!r.ok) error = r.error;
		return r.ok;
	}

	LuaModPtr LoadLua(const MirageApi* api, MirageMod* mod, const std::filesystem::path& file, std::string& error)
	{
		LuaModPtr m(new LuaMod(api, mod));
		if (!m->Load(file, error)) return nullptr;
		return m;
	}
}
