// Lua mods: the `mirage` table, bound to MirageApi only (runtime/mod_lua.h).
//
// One state per mod. Callbacks come from the game thread (on_tick) and the render thread (on_render), so every entry
// into the state holds the mod's lock. Each call gets the state's instruction budget; a callback that errors is
// logged with the mod's name and its module switched off (it would otherwise log the same error every frame).

#include "runtime/mod_lua.h"

#include "runtime/lua_vm.h"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <cstring>
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
		constexpr const char* kProxyMeta = "mirage.value";
		constexpr const char* kScriptMeta = "mirage.script";		// A function's bytecode, open for patching		// An object, or a struct in an object or a call
		constexpr const char* kCallMeta = "mirage.call";		// What a hook gets: valid during the hook only
		constexpr const char* kCallbackMeta = "mirage.callback";	// A subscription or a hook: :remove()
		enum ProxyKind { kObject = 0, kInObject = 1, kInCall = 2 };
		struct Proxy			// uservalue 1: the path (string); 2: the call box (kInCall)
		{
			int kind;
			void* handle;			// MirageObject* or MirageCall*
		};
		struct CallBox
		{
			MirageCall* call;		// nullptr once the hook returned
		};

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

		// ---- Game objects, events, hooks (step 6) ------------------------------------------------------------------
		struct Callback			// An event subscription or a function hook's Lua function
		{
			LuaMod* owner = nullptr;
			int fn = LUA_NOREF;
			MirageSubscription* subscription = nullptr;
			MirageHook* hook = nullptr;
			std::string what;
			bool failed = false;
		};
		static void MIRAGE_CALL OnEvent(void* user, const MirageEvent* e);
		static void MIRAGE_CALL OnHook(void* user, MirageCall* call);
		void Fail(Callback& c, const std::string& error);
		static int On(lua_State* L);
		static int HookFn(lua_State* L);
		static int CallbackRemove(lua_State* L);
		static int Pawn(lua_State* L);
		static int Controller(lua_State* L);
		static int World(lua_State* L);
		static int Find(lua_State* L);
		static int Name(lua_State* L);
		static int ClassName(lua_State* L);
		static int IsA(lua_State* L);
		static int CallGame(lua_State* L);
		static int ProxyIndex(lua_State* L);
		static int ProxyNewIndex(lua_State* L);
		static int ProxyEq(lua_State* L);
		static int ProxyToString(lua_State* L);
		static int CallIndex(lua_State* L);
		static int CallSkip(lua_State* L);
		static MirageScript* CheckScript(lua_State* L);
		static bool ToToken(lua_State* L, int index, MirageToken& t);
		static int ScriptOpen(lua_State* L);
		static int ScriptRestore(lua_State* L);
		static int ScriptCount(lua_State* L);
		static int ScriptGet(lua_State* L);
		static int ScriptSet(lua_State* L);
		static int ScriptAdd(lua_State* L);
		static int ScriptChildren(lua_State* L);
		static int ScriptSetChildren(lua_State* L);
		static int ScriptApply(lua_State* L);
		static int ScriptDump(lua_State* L);
		static int ScriptClose(lua_State* L);
		static void PushObject(lua_State* L, MirageObject* o);
		static void PushProxy(lua_State* L, int kind, void* handle, const std::string& path, int owner);
		static void PushValue(lua_State* L, const MirageValue& v, int kind, void* handle, const std::string& path, int owner);
		static bool ToValue(lua_State* L, int index, MirageValue& out);
		static MirageObject* CheckObject(lua_State* L, int index);
		int Set(lua_State* L, int kind, void* handle, const std::string& path, int value);

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
		std::vector<std::unique_ptr<Callback>> callbacks_;		// Events and hooks (kept until the mod unloads)
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

	// ---- Game objects --------------------------------------------------------------------------------------------
	void LuaMod::PushProxy(lua_State* L, int kind, void* handle, const std::string& path, int owner)
	{
		auto* p = static_cast<Proxy*>(lua_newuserdatauv(L, sizeof(Proxy), 2));
		p->kind = kind;
		p->handle = handle;
		lua_pushlstring(L, path.data(), path.size());
		lua_setiuservalue(L, -2, 1);
		if (owner) {
			lua_pushvalue(L, owner);
			lua_setiuservalue(L, -2, 2);
		}
		luaL_setmetatable(L, kProxyMeta);
	}

	void LuaMod::PushObject(lua_State* L, MirageObject* o)
	{
		if (!o) lua_pushnil(L);
		else PushProxy(L, kObject, o, "", 0);
	}

	// A value as Lua sees it: numbers, booleans, strings, objects; a struct is a proxy reaching into it
	void LuaMod::PushValue(lua_State* L, const MirageValue& v, int kind, void* handle, const std::string& path, int owner)
	{
		switch (v.type) {
			case MIRAGE_VALUE_BOOL: lua_pushboolean(L, v.i); break;
			case MIRAGE_VALUE_INT: lua_pushinteger(L, v.i); break;
			case MIRAGE_VALUE_FLOAT: lua_pushnumber(L, v.f); break;
			case MIRAGE_VALUE_OBJECT: PushObject(L, v.o); break;
			case MIRAGE_VALUE_TEXT: lua_pushstring(L, v.text); break;
			case MIRAGE_VALUE_STRUCT: PushProxy(L, kind == kObject ? kInObject : kind, handle, path, owner); break;
			default: lua_pushnil(L); break;
		}
	}

	MirageObject* LuaMod::CheckObject(lua_State* L, int index)
	{
		auto* p = static_cast<Proxy*>(luaL_testudata(L, index, kProxyMeta));
		if (!p || p->kind != kObject) luaL_typeerror(L, index, "game object");
		return static_cast<MirageObject*>(p->handle);
	}

	bool LuaMod::ToValue(lua_State* L, int index, MirageValue& out)
	{
		out = MirageValue{};
		switch (lua_type(L, index)) {
			case LUA_TBOOLEAN: out.type = MIRAGE_VALUE_BOOL; out.i = lua_toboolean(L, index); return true;
			case LUA_TNUMBER:
				if (lua_isinteger(L, index)) {
					out.type = MIRAGE_VALUE_INT;
					out.i = static_cast<int32_t>(lua_tointeger(L, index));
				} else {
					out.type = MIRAGE_VALUE_FLOAT;
					out.f = static_cast<float>(lua_tonumber(L, index));
				}
				return true;
			case LUA_TSTRING: {
				out.type = MIRAGE_VALUE_TEXT;
				std::size_t n = 0;
				const char* t = lua_tolstring(L, index, &n);
				std::memcpy(out.text, t, std::min<std::size_t>(n, sizeof out.text - 1));
				return true;
			}
			case LUA_TNIL: out.type = MIRAGE_VALUE_OBJECT; return true;
			case LUA_TUSERDATA:
				if (auto* p = static_cast<Proxy*>(luaL_testudata(L, index, kProxyMeta)); p && p->kind == kObject) {
					out.type = MIRAGE_VALUE_OBJECT;
					out.o = static_cast<MirageObject*>(p->handle);
					return true;
				}
				return false;
			default: return false;
		}
	}

	// Writes the Lua value at `value` to `path`; a table sets the struct's members one by one
	int LuaMod::Set(lua_State* L, int kind, void* handle, const std::string& path, int value)
	{
		if (lua_type(L, value) == LUA_TTABLE) {
			lua_pushnil(L);
			while (lua_next(L, value)) {
				if (lua_type(L, -2) == LUA_TSTRING) Set(L, kind, handle, path + "." + lua_tostring(L, -2), lua_gettop(L));
				lua_pop(L, 1);
			}
			return 0;
		}
		MirageValue v;
		if (!ToValue(L, value, v)) return luaL_error(L, "%s: can't be set to a %s", path.c_str(), luaL_typename(L, value));
		const int ok = kind == kInCall ? api_->call_set(static_cast<MirageCall*>(handle), path.c_str(), &v)
									   : api_->object_set(static_cast<MirageObject*>(handle), path.c_str(), &v);
		if (!ok) return luaL_error(L, "%s can't be set (no such member, another type, or the object is gone)", path.c_str());
		return 0;
	}

	namespace
	{
		// The proxy at 1, with its path and owner (a call box, or 0) on the stack
		struct ProxyAt
		{
			Proxy* p;
			std::string path;
			int owner = 0;
		};
		ProxyAt OpenProxy(lua_State* L)
		{
			ProxyAt r{static_cast<Proxy*>(luaL_checkudata(L, 1, kProxyMeta)), {}};
			lua_getiuservalue(L, 1, 1);
			r.path = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
			lua_pop(L, 1);
			if (r.p->kind == kInCall) {
				lua_getiuservalue(L, 1, 2);
				auto* box = static_cast<CallBox*>(lua_touserdata(L, -1));
				if (!box || !box->call) luaL_error(L, "a hook's parameters are only there while the hook runs");
				r.owner = lua_gettop(L);
			}
			return r;
		}
		std::string Join(const std::string& path, const char* key) { return path.empty() ? key : path + "." + key; }
	}

	int LuaMod::ProxyIndex(lua_State* L)
	{
		LuaMod& self = Self(L);
		const ProxyAt at = OpenProxy(L);
		const std::string path = Join(at.path, luaL_checkstring(L, 2));
		MirageValue v;
		const int ok = at.p->kind == kInCall ? self.api_->call_get(static_cast<MirageCall*>(at.p->handle), path.c_str(), &v)
											 : self.api_->object_get(static_cast<MirageObject*>(at.p->handle), path.c_str(), &v);
		if (!ok) {
			lua_pushnil(L);
			return 1;
		}
		PushValue(L, v, at.p->kind, at.p->handle, path, at.owner);
		return 1;
	}

	int LuaMod::ProxyNewIndex(lua_State* L)
	{
		const ProxyAt at = OpenProxy(L);
		return Self(L).Set(L, at.p->kind, at.p->handle, Join(at.path, luaL_checkstring(L, 2)), 3);
	}

	int LuaMod::ProxyEq(lua_State* L)
	{
		auto* a = static_cast<Proxy*>(luaL_testudata(L, 1, kProxyMeta));
		auto* b = static_cast<Proxy*>(luaL_testudata(L, 2, kProxyMeta));
		lua_pushboolean(L, a && b && a->kind == kObject && b->kind == kObject && a->handle == b->handle);
		return 1;
	}

	int LuaMod::ProxyToString(lua_State* L)
	{
		LuaMod& self = Self(L);
		const ProxyAt at = OpenProxy(L);
		if (at.p->kind == kObject) {
			char name[128];
			self.api_->object_name(static_cast<MirageObject*>(at.p->handle), name, sizeof name);
			lua_pushstring(L, name);
		} else {
			lua_pushfstring(L, "<%s>", at.path.c_str());
		}
		return 1;
	}

	int LuaMod::Pawn(lua_State* L) { PushObject(L, Self(L).api_->player_pawn()); return 1; }
	int LuaMod::Controller(lua_State* L) { PushObject(L, Self(L).api_->player_controller()); return 1; }
	int LuaMod::World(lua_State* L) { PushObject(L, Self(L).api_->world_info()); return 1; }
	int LuaMod::Find(lua_State* L) { PushObject(L, Self(L).api_->object_find(luaL_checkstring(L, 1))); return 1; }

	int LuaMod::Name(lua_State* L)
	{
		char name[256];
		Self(L).api_->object_name(CheckObject(L, 1), name, sizeof name);
		lua_pushstring(L, name);
		return 1;
	}
	int LuaMod::ClassName(lua_State* L)
	{
		char name[256];
		Self(L).api_->object_class_name(CheckObject(L, 1), name, sizeof name);
		lua_pushstring(L, name);
		return 1;
	}
	int LuaMod::IsA(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->object_is_a(CheckObject(L, 1), luaL_checkstring(L, 2)));
		return 1;
	}

	// mirage.call(object, "Function", { Param = value, ... }): its return value (true if it returns nothing)
	int LuaMod::CallGame(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageObject* o = CheckObject(L, 1);
		const char* fn = luaL_checkstring(L, 2);
		MirageCall* call = self.api_->call_begin(o, fn);
		if (!call) return luaL_error(L, "mirage.call: no function %s on this object", fn);
		if (lua_istable(L, 3)) {
			lua_pushnil(L);
			while (lua_next(L, 3)) {
				if (lua_type(L, -2) == LUA_TSTRING) {
					const std::string param = lua_tostring(L, -2);
					MirageValue probe;
					if (!self.api_->call_get(call, param.c_str(), &probe)) {
						self.api_->call_end(call);
						return luaL_error(L, "mirage.call: %s has no parameter %s", fn, param.c_str());
					}
					self.Set(L, kInCall, call, param, lua_gettop(L));
				}
				lua_pop(L, 1);
			}
		}
		if (!self.api_->call_invoke(call)) {
			self.api_->call_end(call);
			return luaL_error(L, "mirage.call: %s didn't run", fn);
		}
		MirageValue ret;
		if (self.api_->call_get(call, "ReturnValue", &ret) && ret.type != MIRAGE_VALUE_STRUCT) PushValue(L, ret, kInCall, call, "", 0);
		else lua_pushboolean(L, 1);
		self.api_->call_end(call);
		return 1;
	}

	// ---- Events and hooks ------------------------------------------------------------------------------------------
	void LuaMod::Fail(Callback& c, const std::string& error)
	{
		c.failed = true;
		Log(MIRAGE_LOG_ERROR, c.what + ": " + error + " (removed)");
		if (c.hook) api_->hook_remove(c.hook);
		if (c.subscription) api_->event_unsubscribe(c.subscription);
	}

	void MIRAGE_CALL LuaMod::OnEvent(void* user, const MirageEvent* e)
	{
		auto* c = static_cast<Callback*>(user);
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		if (c->failed) return;
		lua_State* L = self.state_.Get();
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		int n = 0;
		switch (e->type) {
			case MIRAGE_EVENT_TICK: lua_pushnumber(L, e->number); n = 1; break;
			case MIRAGE_EVENT_LEVEL_LOAD: lua_pushboolean(L, e->value); lua_pushstring(L, e->text ? e->text : ""); n = 2; break;
			case MIRAGE_EVENT_DEATH: lua_pushboolean(L, e->value); n = 1; break;
			case MIRAGE_EVENT_PLAYER_STATE: lua_pushinteger(L, e->value); lua_pushinteger(L, e->value2); n = 2; break;
			case MIRAGE_EVENT_KEY: lua_pushinteger(L, e->value); n = 1; break;
			default: break;
		}
		const lua::Result r = self.state_.Call(n, 0);
		if (!r.ok) self.Fail(*c, r.error);
	}

	void MIRAGE_CALL LuaMod::OnHook(void* user, MirageCall* call)
	{
		auto* c = static_cast<Callback*>(user);
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		if (c->failed) return;
		lua_State* L = self.state_.Get();
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		auto* box = static_cast<CallBox*>(lua_newuserdatauv(L, sizeof(CallBox), 0));
		box->call = call;
		luaL_setmetatable(L, kCallMeta);
		lua_pushvalue(L, -1);
		const int boxRef = luaL_ref(L, LUA_REGISTRYINDEX);	// To void it after the hook, wherever Lua kept it
		const lua::Result r = self.state_.Call(1, 0);
		lua_rawgeti(L, LUA_REGISTRYINDEX, boxRef);
		static_cast<CallBox*>(lua_touserdata(L, -1))->call = nullptr;
		lua_pop(L, 1);
		luaL_unref(L, LUA_REGISTRYINDEX, boxRef);
		if (!r.ok) self.Fail(*c, r.error);
	}

	// call.object, call.params, call.func; call:skip()
	int LuaMod::CallIndex(lua_State* L)
	{
		LuaMod& self = Self(L);
		auto* box = static_cast<CallBox*>(luaL_checkudata(L, 1, kCallMeta));
		const std::string key = luaL_checkstring(L, 2);
		if (key == "skip") {
			lua_pushlightuserdata(L, &self);
			lua_pushcclosure(L, CallSkip, 1);
			return 1;
		}
		if (!box->call) return luaL_error(L, "a hook's call is only there while the hook runs");
		if (key == "object") PushObject(L, self.api_->call_object(box->call));
		else if (key == "params") PushProxy(L, kInCall, box->call, "", 1);
		else if (key == "func") lua_pushstring(L, self.api_->call_function(box->call));
		else lua_pushnil(L);
		return 1;
	}

	int LuaMod::CallSkip(lua_State* L)
	{
		auto* box = static_cast<CallBox*>(luaL_checkudata(L, 1, kCallMeta));
		if (!box->call) return luaL_error(L, "a hook's call is only there while the hook runs");
		Self(L).api_->call_skip(box->call);
		return 0;
	}

	// mirage.on("tick" | "level_load" | "death" | "player_state" | "key", function)
	int LuaMod::On(lua_State* L)
	{
		LuaMod& self = Self(L);
		static const char* const kEvents[] = {"tick", "level_load", "death", "player_state", "key", "state_restored", nullptr};
		const int type = luaL_checkoption(L, 1, nullptr, kEvents);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("mirage.on ") + kEvents[type];
		lua_pushvalue(L, 2);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		c->subscription = self.api_->event_subscribe(self.mod_, type, &OnEvent, c.get());
		if (!c->subscription) return luaL_error(L, "mirage.on: %s can't be subscribed to", kEvents[type]);
		auto** box = static_cast<Callback**>(lua_newuserdatauv(L, sizeof(Callback*), 0));
		*box = c.get();
		luaL_setmetatable(L, kCallbackMeta);
		self.callbacks_.push_back(std::move(c));
		return 1;
	}

	// mirage.hook("Engine.Actor.SetLocation", function(call) ... end, { when = "before", priority = 0 })
	int LuaMod::HookFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* function = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		int when = MIRAGE_HOOK_BEFORE, priority = 0;
		if (lua_istable(L, 3)) {
			lua_getfield(L, 3, "when");
			static const char* const kWhen[] = {"before", "after", "replace", nullptr};
			if (!lua_isnil(L, -1)) when = luaL_checkoption(L, -1, nullptr, kWhen);
			lua_pop(L, 1);
			lua_getfield(L, 3, "priority");
			priority = static_cast<int>(luaL_optinteger(L, -1, 0));
			lua_pop(L, 1);
		}
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("hook of ") + function;
		lua_pushvalue(L, 2);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		c->hook = self.api_->hook_function(self.mod_, function, when, priority, &OnHook, c.get());
		if (!c->hook) return luaL_error(L, "mirage.hook: %s can't be hooked (see the log)", function);
		auto** box = static_cast<Callback**>(lua_newuserdatauv(L, sizeof(Callback*), 0));
		*box = c.get();
		luaL_setmetatable(L, kCallbackMeta);
		self.callbacks_.push_back(std::move(c));
		return 1;
	}

	int LuaMod::CallbackRemove(lua_State* L)
	{
		auto** box = static_cast<Callback**>(luaL_checkudata(L, 1, kCallbackMeta));
		Callback& c = **box;
		if (c.failed) return 0;
		c.failed = true;
		if (c.hook) Self(L).api_->hook_remove(c.hook);
		if (c.subscription) Self(L).api_->event_unsubscribe(c.subscription);
		return 0;
	}

	// ---- mirage.script: script patching (step 8) -------------------------------------------------------------------
	//
	//   local s = mirage.script.open("TdGame.TdMove_Jump.StartMove")
	//   print(s:dump())                         -- every token: index, offset, kind, operands
	//   local t = s:get(12)                     -- {kind = "FloatConst", float = 10.0, children = {...}, ...}
	//   t.float = 20.0; s:set(12, t)
	//   local k = s:add{kind = "FloatConst", float = 1.5}
	//   s:set_children(9, {s:children(9)[1], k})
	//   s:apply()                               -- in place until mirage.script.restore(path) or the mod's unload
	//
	// Indices are 1-based; children(0) / set_children(0, ...) are the top-level statements.

	MirageScript* LuaMod::CheckScript(lua_State* L)
	{
		auto** box = static_cast<MirageScript**>(luaL_checkudata(L, 1, kScriptMeta));
		if (!*box) luaL_error(L, "the script was closed");
		return *box;
	}

	int LuaMod::ScriptOpen(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageScript* s = self.api_->script_open(self.mod_, luaL_checkstring(L, 1));
		if (!s) {
			lua_pushnil(L);
			return 1;
		}
		auto** box = static_cast<MirageScript**>(lua_newuserdatauv(L, sizeof(MirageScript*), 0));
		*box = s;
		luaL_setmetatable(L, kScriptMeta);
		return 1;
	}

	int LuaMod::ScriptRestore(lua_State* L)
	{
		LuaMod& self = Self(L);
		lua_pushboolean(L, self.api_->script_restore(self.mod_, luaL_checkstring(L, 1)));
		return 1;
	}

	int LuaMod::ScriptCount(lua_State* L)
	{
		lua_pushinteger(L, Self(L).api_->script_count(CheckScript(L)));
		return 1;
	}

	int LuaMod::ScriptGet(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		MirageToken t{};
		t.size = sizeof t;
		if (!api.script_get(s, index, &t)) {
			lua_pushnil(L);
			return 1;
		}
		lua_newtable(L);
		lua_pushstring(L, api.script_kind_name(t.kind));
		lua_setfield(L, -2, "kind");
		lua_pushinteger(L, t.native_index);
		lua_setfield(L, -2, "native");
		lua_pushinteger(L, t.int_value);
		lua_setfield(L, -2, "int");
		lua_pushnumber(L, t.floats[0]);
		lua_setfield(L, -2, "float");
		lua_createtable(L, 3, 0);
		for (int i = 0; i < 3; ++i) {
			lua_pushnumber(L, t.floats[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "floats");
		lua_createtable(L, 3, 0);
		for (int i = 0; i < 3; ++i) {
			lua_pushinteger(L, t.ints[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "ints");
		PushObject(L, t.object);
		lua_setfield(L, -2, "object");
		PushObject(L, t.object2);
		lua_setfield(L, -2, "object2");
		char objectName[256] = "";
		if (t.object) api.object_name(t.object, objectName, sizeof objectName);
		lua_pushstring(L, objectName);
		lua_setfield(L, -2, "object_name");
		lua_pushstring(L, t.name);
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, t.word);
		lua_setfield(L, -2, "word");
		lua_pushinteger(L, t.word2);
		lua_setfield(L, -2, "word2");
		lua_pushinteger(L, t.byte_value);
		lua_setfield(L, -2, "byte");
		lua_pushinteger(L, t.byte_value2);
		lua_setfield(L, -2, "byte2");
		lua_pushinteger(L, t.memory_offset);
		lua_setfield(L, -2, "offset");
		lua_pushstring(L, t.text);
		lua_setfield(L, -2, "text");
		// Its operands, as indices
		const int32_t n = api.script_children(s, index, nullptr, 0);
		std::vector<int32_t> kids(static_cast<std::size_t>(std::max(n, 0)));
		if (n > 0) api.script_children(s, index, kids.data(), n);
		lua_createtable(L, n, 0);
		for (int32_t i = 0; i < n; ++i) {
			lua_pushinteger(L, kids[static_cast<std::size_t>(i)] + 1);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "children");
		return 1;
	}

	// A token table (as script:get gives, or the fields the mod sets) into a MirageToken; `t` starts as the current one
	bool LuaMod::ToToken(lua_State* L, int index, MirageToken& t)
	{
		const MirageApi& api = *Self(L).api_;
		luaL_checktype(L, index, LUA_TTABLE);
		const auto field = [&](const char* name) { return lua_getfield(L, index, name); };
		if (field("kind") == LUA_TSTRING) {
			const int32_t k = api.script_kind(lua_tostring(L, -1));
			if (k < 0) return luaL_error(L, "no token kind '%s'", lua_tostring(L, -1)), false;
			t.kind = k;
		}
		lua_pop(L, 1);
		const auto integer = [&](const char* name, auto& out) {
			if (field(name) == LUA_TNUMBER) out = static_cast<std::remove_reference_t<decltype(out)>>(lua_tointeger(L, -1));
			lua_pop(L, 1);
		};
		integer("native", t.native_index);
		integer("int", t.int_value);
		integer("word", t.word);
		integer("word2", t.word2);
		integer("byte", t.byte_value);
		integer("byte2", t.byte_value2);
		if (field("float") == LUA_TNUMBER) t.floats[0] = static_cast<float>(lua_tonumber(L, -1));
		lua_pop(L, 1);
		for (const auto& [name, isFloat] : {std::pair{"floats", true}, std::pair{"ints", false}}) {
			if (field(name) == LUA_TTABLE)
				for (int i = 0; i < 3; ++i) {
					if (lua_rawgeti(L, -1, i + 1) == LUA_TNUMBER) {
						if (isFloat) t.floats[i] = static_cast<float>(lua_tonumber(L, -1));
						else t.ints[i] = static_cast<int32_t>(lua_tointeger(L, -1));
					}
					lua_pop(L, 1);
				}
			lua_pop(L, 1);
		}
		for (const auto& [name, out] : {std::pair{"object", &t.object}, std::pair{"object2", &t.object2}}) {
			const int type = field(name);
			if (type == LUA_TNIL) {
			} else if (type == LUA_TSTRING) {		// By full name: "FloatProperty Core.Object.Vector.Z"
				*out = api.object_find(lua_tostring(L, -1));
				if (!*out) return luaL_error(L, "no object '%s'", lua_tostring(L, -1)), false;
			} else if (type == LUA_TBOOLEAN && !lua_toboolean(L, -1)) {
				*out = nullptr;
			} else {
				*out = CheckObject(L, lua_gettop(L));
			}
			lua_pop(L, 1);
		}
		const auto text = [&](const char* name, char* out, std::size_t capacity) {
			if (field(name) == LUA_TSTRING) {
				std::size_t n = 0;
				const char* s = lua_tolstring(L, -1, &n);
				n = std::min(n, capacity - 1);
				std::memcpy(out, s, n);
				out[n] = '\0';
			}
			lua_pop(L, 1);
		};
		text("name", t.name, sizeof t.name);
		text("text", t.text, sizeof t.text);
		if (field("target") == LUA_TNUMBER) t.target = static_cast<int32_t>(lua_tointeger(L, -1)) - 1;
		else if (lua_type(L, -1) == LUA_TSTRING && std::string_view(lua_tostring(L, -1)) == "end") t.target = -2;
		lua_pop(L, 1);
		return true;
	}

	int LuaMod::ScriptSet(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		MirageToken t{};
		t.size = sizeof t;
		if (!api.script_get(s, index, &t)) return luaL_error(L, "no token %d", index + 1);
		ToToken(L, 3, t);
		lua_pushboolean(L, api.script_set(s, index, &t));
		return 1;
	}

	int LuaMod::ScriptAdd(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		MirageToken t{};
		t.size = sizeof t;
		t.target = -1;
		ToToken(L, 2, t);
		const int32_t index = api.script_add(s, &t);
		if (index < 0) return luaL_error(L, "the token can't be added (the log says why)");
		// Its operands, if given
		if (lua_getfield(L, 2, "children") == LUA_TTABLE) {
			std::vector<int32_t> kids;
			for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TNUMBER; ++i) {
				kids.push_back(static_cast<int32_t>(lua_tointeger(L, -1)) - 1);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
			if (!api.script_set_children(s, index, kids.data(), static_cast<int32_t>(kids.size())))
				return luaL_error(L, "the token's children are out of range");
		}
		lua_pop(L, 1);
		lua_pushinteger(L, index + 1);
		return 1;
	}

	int LuaMod::ScriptChildren(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		const int32_t n = api.script_children(s, index, nullptr, 0);
		if (n < 0) return luaL_error(L, "no token %d", index + 1);
		std::vector<int32_t> kids(static_cast<std::size_t>(n));
		if (n > 0) api.script_children(s, index, kids.data(), n);
		lua_createtable(L, n, 0);
		for (int32_t i = 0; i < n; ++i) {
			lua_pushinteger(L, kids[static_cast<std::size_t>(i)] + 1);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}

	int LuaMod::ScriptSetChildren(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		luaL_checktype(L, 3, LUA_TTABLE);
		std::vector<int32_t> kids;
		for (lua_Integer i = 1; lua_rawgeti(L, 3, i) == LUA_TNUMBER; ++i) {
			kids.push_back(static_cast<int32_t>(lua_tointeger(L, -1)) - 1);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		lua_pushboolean(L, api.script_set_children(s, index, kids.data(), static_cast<int32_t>(kids.size())));
		return 1;
	}

	int LuaMod::ScriptApply(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->script_apply(CheckScript(L)));
		return 1;
	}

	int LuaMod::ScriptDump(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const uint32_t n = api.script_dump(s, nullptr, 0);
		std::string text(n + 1, '\0');
		api.script_dump(s, text.data(), n + 1);
		text.resize(n);
		lua_pushlstring(L, text.data(), text.size());
		return 1;
	}

	int LuaMod::ScriptClose(lua_State* L)
	{
		auto** box = static_cast<MirageScript**>(luaL_checkudata(L, 1, kScriptMeta));
		if (*box) Self(L).api_->script_close(*box);
		*box = nullptr;
		return 0;
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

		luaL_newmetatable(L, kProxyMeta);
		const luaL_Reg proxyFns[] = {{"__index", ProxyIndex}, {"__newindex", ProxyNewIndex}, {"__eq", ProxyEq},
			{"__tostring", ProxyToString}, {nullptr, nullptr}};
		set(proxyFns);
		lua_pop(L, 1);

		luaL_newmetatable(L, kCallMeta);
		const luaL_Reg callFns[] = {{"__index", CallIndex}, {nullptr, nullptr}};
		set(callFns);
		lua_pop(L, 1);

		luaL_newmetatable(L, kCallbackMeta);
		lua_newtable(L);
		const luaL_Reg callbackFns[] = {{"remove", CallbackRemove}, {nullptr, nullptr}};
		set(callbackFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		luaL_newmetatable(L, kScriptMeta);
		lua_newtable(L);
		const luaL_Reg scriptFns[] = {{"count", ScriptCount}, {"get", ScriptGet}, {"set", ScriptSet}, {"add", ScriptAdd},
			{"children", ScriptChildren}, {"set_children", ScriptSetChildren}, {"apply", ScriptApply}, {"dump", ScriptDump},
			{"close", ScriptClose}, {nullptr, nullptr}};
		set(scriptFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		lua_newtable(L);	// mirage
		const luaL_Reg fns[] = {{"log", LuaLog<MIRAGE_LOG_INFO>}, {"info", LuaLog<MIRAGE_LOG_INFO>},
			{"warn", LuaLog<MIRAGE_LOG_WARN>}, {"error", LuaLog<MIRAGE_LOG_ERROR>}, {"module", LuaModule},
			{"setting", FindSetting}, {"rgba", Rgba},
			{"on", On}, {"hook", HookFn}, {"pawn", Pawn}, {"controller", Controller}, {"world", World}, {"find", Find},
			{"name", Name}, {"class", ClassName}, {"is_a", IsA}, {"call", CallGame}, {nullptr, nullptr}};
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

		lua_newtable(L);
		const luaL_Reg scriptLib[] = {{"open", ScriptOpen}, {"restore", ScriptRestore}, {nullptr, nullptr}};
		set(scriptLib);
		lua_setfield(L, -2, "script");
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
