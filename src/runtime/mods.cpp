#include "runtime/mods.h"

#include "core/module.h"
#include "core/setting.h"
#include "runtime/manifest.h"
#include "runtime/mod_lua.h"

#include <imgui.h>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

// The runtime's record of a mod: what api.h calls MirageMod
struct MirageMod
{
	mirage::mods::Manifest manifest;
	std::string id, version, dir;					// Kept as C strings for the API
	HMODULE dll = nullptr;
	MirageModUnloadFn unload = nullptr;
	mirage::mods::LuaModPtr lua;
	std::vector<Module*> modules;					// Registered by it (owned by the menu's module list)
	std::map<std::string, ctl::Setting*> declared;	// Its mod.toml settings that were made, by key
	std::mutex configLock;
	toml::value config;
	bool configRead = false;
};

namespace mirage::mods
{
	namespace
	{
		std::string Utf8(const std::filesystem::path& p)
		{
			const std::u8string s = p.u8string();
			return std::string(s.begin(), s.end());
		}
		std::filesystem::path FromUtf8(const std::string& s)
		{
			return std::filesystem::path(std::u8string(s.begin(), s.end()));
		}
		const char* Str(const char* s) { return s ? s : ""; }

		// ---- Calling into a mod: a crash (an access violation, a C++ exception thrown across the boundary) is caught
		// and reported instead of taking the game down. Plain function: __try can't share a frame with C++ objects.
		DWORD Guarded(MirageCallback cb, void* user)
		{
			__try {
				cb(user);
				return 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return GetExceptionCode();
			}
		}
		DWORD GuardedLoad(MirageModLoadFn load, const MirageApi* api, MirageMod* mod, int32_t* result)
		{
			__try {
				*result = load(api, mod);
				return 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return GetExceptionCode();
			}
		}
		DWORD GuardedUnload(MirageModUnloadFn unload)
		{
			__try {
				unload();
				return 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return GetExceptionCode();
			}
		}

		// ---- A mod's module ----------------------------------------------------------------------------------------
		class ModModule final : public Module
		{
		public:
			ModModule(MirageMod* mod, const MirageModuleDesc& d)
				: Module(d.name, d.tab && *d.tab ? d.tab : "Mods", Str(d.description), d.order ? d.order : 100), mod_(mod),
				  desc_(d), action_((d.flags & MIRAGE_MODULE_ACTION) != 0)
			{
				key = d.key;
				enabledByDefault = (d.flags & MIRAGE_MODULE_ENABLED_BY_DEFAULT) != 0;
			}

			bool IsAction() const override { return action_; }
			void OnEnable() override { Call(desc_.on_enable, "on_enable"); }
			void OnDisable() override { Call(desc_.on_disable, "on_disable"); }
			void OnTick() override { Call(desc_.on_tick, "on_tick"); }
			void OnRender() override { Call(desc_.on_render, "on_render"); }
			void OnTrigger() override { Call(desc_.on_trigger, "on_trigger"); }

			MirageMod* Mod() const { return mod_; }
			std::vector<std::unique_ptr<ctl::Setting>> owned;	// The settings the mod made

		private:
			void Call(MirageCallback cb, const char* what)
			{
				if (!cb || crashed_) return;
				if (const DWORD code = Guarded(cb, desc_.user)) {
					crashed_ = true;	// Not called again: whatever it was doing is in an unknown state
					spdlog::error("[mod {}] {}: {} crashed (exception 0x{:08X}): the module is stopped", mod_->id, Name(),
								  what, code);
					if (Enabled() && std::strcmp(what, "on_disable") != 0) SetEnabled(false);
				}
			}

			MirageMod* mod_;
			MirageModuleDesc desc_;
			bool action_;
			bool crashed_ = false;
		};

		ctl::Setting* SettingOf(MirageSetting* s) { return reinterpret_cast<ctl::Setting*>(s); }
		MirageSetting* Handle(ctl::Setting* s) { return reinterpret_cast<MirageSetting*>(s); }
		ModModule* ModuleOf(MirageModule* m) { return reinterpret_cast<ModModule*>(m); }

		template <class T, class... Args>
		MirageSetting* MakeSetting(MirageModule* module, const char* name, Args&&... args)
		{
			ModModule* m = ModuleOf(module);
			if (!m || !name || !*name) return nullptr;
			if (m->FindSetting(name)) {
				spdlog::warn("[mod {}] {}: a setting named '{}' already exists", m->Mod()->id, m->Name(), name);
				return nullptr;
			}
			auto s = std::make_unique<T>(m, name, std::forward<Args>(args)...);	// Registers itself with m
			ctl::Setting* raw = s.get();
			m->owned.push_back(std::move(s));
			return Handle(raw);
		}

		uint32_t CopyOut(const std::string& s, char* buffer, uint32_t capacity)
		{
			if (buffer && capacity) {
				const std::size_t n = std::min<std::size_t>(s.size(), capacity - 1);
				std::memcpy(buffer, s.data(), n);
				buffer[n] = '\0';
			}
			return static_cast<uint32_t>(s.size());
		}

		void ReadConfig(MirageMod* mod)
		{
			if (mod->configRead) return;
			mod->configRead = true;
			mod->config = toml::table{};
			const auto file = FromUtf8(mod->dir) / "config.toml";
			if (!std::filesystem::exists(file)) return;
			try {
				mod->config = toml::parse(file);
			} catch (const std::exception& e) {
				spdlog::warn("[mod {}] config.toml can't be read, starting empty: {}", mod->id, e.what());
				mod->config = toml::table{};
			}
		}

		// The settings mod.toml declares for this module ([settings.<key>] with module = its name, or no module and
		// this is the mod's first module), in the file's order
		void AddDeclaredSettings(MirageMod& mod, ModModule& m, bool firstModule)
		{
			using T = SettingDecl::Type;
			for (const SettingDecl& d : mod.manifest.settings) {
				if (mod.declared.count(d.key)) continue;
				if (d.module.empty() ? !firstModule : d.module != m.Name()) continue;
				if (m.FindSetting(d.name)) {
					spdlog::warn("[mod {}] {}: setting '{}' from mod.toml clashes with one of the same name", mod.id, m.Name(), d.name);
					continue;
				}
				std::unique_ptr<ctl::Setting> s;
				switch (d.type) {
					case T::Bool: s = std::make_unique<ctl::Checkbox>(&m, d.name, d.boolDefault, d.description); break;
					case T::Int:
						s = std::make_unique<ctl::SliderInt>(&m, d.name, static_cast<int>(d.intDefault), static_cast<int>(d.min),
															 static_cast<int>(d.max), std::string("%d"), 0, d.description);
						break;
					case T::Float:
						s = std::make_unique<ctl::Slider>(&m, d.name, static_cast<float>(d.floatDefault), static_cast<float>(d.min),
														  static_cast<float>(d.max), d.format.empty() ? std::string("%.2f") : d.format,
														  0.f, d.description);
						break;
					case T::String: s = std::make_unique<ctl::Text>(&m, d.name, d.stringDefault, d.description); break;
					case T::Choice:
						s = std::make_unique<ctl::Combo>(&m, d.name, d.options, static_cast<int>(d.intDefault), d.description);
						break;
					case T::Key: s = std::make_unique<ctl::KeyHold>(&m, d.name, static_cast<int>(d.intDefault), d.description); break;
				}
				mod.declared[d.key] = s.get();
				m.owned.push_back(std::move(s));
			}
		}

		// ---- The table -------------------------------------------------------------------------------------------
		void MIRAGE_CALL Log(MirageMod* mod, int32_t level, const char* text)
		{
			const std::string id = mod ? mod->id : "?";
			switch (level) {
				case MIRAGE_LOG_DEBUG: spdlog::debug("[mod {}] {}", id, Str(text)); break;
				case MIRAGE_LOG_WARN: spdlog::warn("[mod {}] {}", id, Str(text)); break;
				case MIRAGE_LOG_ERROR: spdlog::error("[mod {}] {}", id, Str(text)); break;
				default: spdlog::info("[mod {}] {}", id, Str(text)); break;
			}
		}
		const char* MIRAGE_CALL ModId(MirageMod* mod) { return mod ? mod->id.c_str() : ""; }
		const char* MIRAGE_CALL ModVersion(MirageMod* mod) { return mod ? mod->version.c_str() : ""; }
		const char* MIRAGE_CALL ModDir(MirageMod* mod) { return mod ? mod->dir.c_str() : ""; }

		MirageModule* MIRAGE_CALL ModuleRegister(MirageMod* mod, const MirageModuleDesc* desc)
		{
			if (!mod || !desc) return nullptr;
			// Only the part of the description the mod's header had; the rest stays zero
			MirageModuleDesc d{};
			std::memcpy(&d, desc, std::min<std::size_t>(desc->size, sizeof d));
			d.size = sizeof d;
			if (desc->size < offsetof(MirageModuleDesc, user) || !d.name || !*d.name) {
				spdlog::error("[mod {}] module_register: a module needs a name", mod->id);
				return nullptr;
			}
			if (modules::Find(d.name)) {
				spdlog::error("[mod {}] module_register: a module named '{}' already exists", mod->id, d.name);
				return nullptr;
			}
			Module* m = modules::Add(std::make_unique<ModModule>(mod, d));
			if (!m) return nullptr;
			const bool first = mod->modules.empty();
			mod->modules.push_back(m);
			AddDeclaredSettings(*mod, *static_cast<ModModule*>(m), first);
			return reinterpret_cast<MirageModule*>(static_cast<ModModule*>(m));
		}
		int32_t MIRAGE_CALL ModuleEnabled(MirageModule* m) { return m && ModuleOf(m)->Enabled() ? 1 : 0; }
		void MIRAGE_CALL ModuleSetEnabled(MirageModule* m, int32_t on)
		{
			if (m) ModuleOf(m)->SetEnabled(on != 0);
		}

		MirageSetting* MIRAGE_CALL SettingCheckbox(MirageModule* m, const char* name, int32_t value, const char* tooltip)
		{
			return MakeSetting<ctl::Checkbox>(m, name, value != 0, std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingSlider(MirageModule* m, const char* name, float value, float min, float max,
												 const char* format, const char* tooltip)
		{
			if (!(min < max)) return nullptr;
			return MakeSetting<ctl::Slider>(m, name, value, min, max, std::string(format && *format ? format : "%.2f"),
											0.f, std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingSliderInt(MirageModule* m, const char* name, int32_t value, int32_t min,
													int32_t max, const char* tooltip)
		{
			if (!(min < max)) return nullptr;
			return MakeSetting<ctl::SliderInt>(m, name, int(value), int(min), int(max), std::string("%d"), 0,
											   std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingText(MirageModule* m, const char* name, const char* value, const char* tooltip)
		{
			return MakeSetting<ctl::Text>(m, name, std::string(Str(value)), std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingKey(MirageModule* m, const char* name, int32_t vk, const char* tooltip)
		{
			return MakeSetting<ctl::KeyHold>(m, name, int(vk), std::string(Str(tooltip)));
		}

		int32_t MIRAGE_CALL SettingGetInt(MirageSetting* h)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) return b->Get() ? 1 : 0;
			if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) return i->Get();
			if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) return static_cast<int32_t>(f->Get());
			return 0;
		}
		float MIRAGE_CALL SettingGetFloat(MirageSetting* h)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) return f->Get();
			if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) return static_cast<float>(i->Get());
			if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) return b->Get() ? 1.f : 0.f;
			return 0.f;
		}
		void MIRAGE_CALL SettingSetInt(MirageSetting* h, int32_t v)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) b->Set(v != 0);
			else if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) i->Set(v);
			else if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) f->Set(static_cast<float>(v));
			else return;
			s->Owner().MarkDirty();
		}
		void MIRAGE_CALL SettingSetFloat(MirageSetting* h, float v)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) f->Set(v);
			else if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) i->Set(static_cast<int>(v));
			else if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) b->Set(v != 0.f);
			else return;
			s->Owner().MarkDirty();
		}
		uint32_t MIRAGE_CALL SettingGetText(MirageSetting* h, char* buffer, uint32_t capacity)
		{
			if (auto* c = dynamic_cast<ctl::Combo*>(SettingOf(h))) return CopyOut(c->Selected(), buffer, capacity);
			auto* t = dynamic_cast<ctl::Value<std::string>*>(SettingOf(h));
			return CopyOut(t ? t->Get() : std::string(), buffer, capacity);
		}
		void MIRAGE_CALL SettingSetText(MirageSetting* h, const char* v)
		{
			if (auto* c = dynamic_cast<ctl::Combo*>(SettingOf(h))) {	// A choice: by its option's text
				const int before = c->Get();
				for (int i = 0; i < 4096; ++i) {
					c->Set(i);
					if (c->Get() != i) break;		// Past the last option (clamped)
					if (c->Selected() == Str(v)) {
						c->Owner().MarkDirty();
						return;
					}
				}
				c->Set(before);		// Not one of its options: unchanged
				return;
			}
			if (auto* t = dynamic_cast<ctl::Value<std::string>*>(SettingOf(h))) {
				t->Set(Str(v));
				t->Owner().MarkDirty();
			}
		}
		int32_t MIRAGE_CALL KeyDown(MirageSetting* h)
		{
			auto* k = dynamic_cast<ctl::KeyHold*>(SettingOf(h));
			return k && k->Down() ? 1 : 0;
		}

		uint32_t MIRAGE_CALL ConfigGet(MirageMod* mod, const char* key, const char* fallback, char* buffer,
									   uint32_t capacity)
		{
			std::string value = Str(fallback);
			if (mod && key) {
				std::lock_guard lock(mod->configLock);
				ReadConfig(mod);
				if (mod->config.is_table() && mod->config.contains(key)) {
					const toml::value& v = mod->config.at(key);
					value = v.is_string() ? v.as_string() : toml::format(v);
				}
			}
			return CopyOut(value, buffer, capacity);
		}
		void MIRAGE_CALL ConfigSet(MirageMod* mod, const char* key, const char* value)
		{
			if (!mod || !key || !*key) return;
			std::lock_guard lock(mod->configLock);
			ReadConfig(mod);
			mod->config.as_table()[key] = std::string(Str(value));
			const auto file = FromUtf8(mod->dir) / "config.toml";
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			if (out) out << toml::format(mod->config);
			else spdlog::warn("[mod {}] can't write {}", mod->id, Utf8(file));
		}

		void MIRAGE_CALL ScreenSize(float* w, float* h)
		{
			const ImVec2 s = ImGui::GetCurrentContext() ? ImGui::GetIO().DisplaySize : ImVec2{};
			if (w) *w = s.x;
			if (h) *h = s.y;
		}
		ImDrawList* DrawList() { return ImGui::GetCurrentContext() ? ImGui::GetBackgroundDrawList() : nullptr; }
		void MIRAGE_CALL DrawText(float x, float y, uint32_t color, const char* text)
		{
			if (ImDrawList* dl = DrawList(); dl && text) dl->AddText({x, y}, color, text);
		}
		void MIRAGE_CALL DrawLine(float x1, float y1, float x2, float y2, uint32_t color, float thickness)
		{
			if (ImDrawList* dl = DrawList()) dl->AddLine({x1, y1}, {x2, y2}, color, thickness);
		}
		void MIRAGE_CALL DrawRect(float x1, float y1, float x2, float y2, uint32_t color, float thickness)
		{
			if (ImDrawList* dl = DrawList()) dl->AddRect({x1, y1}, {x2, y2}, color, 0.f, 0, thickness);
		}
		void MIRAGE_CALL DrawRectFilled(float x1, float y1, float x2, float y2, uint32_t color)
		{
			if (ImDrawList* dl = DrawList()) dl->AddRectFilled({x1, y1}, {x2, y2}, color);
		}
		void MIRAGE_CALL TextSize(const char* text, float* w, float* h)
		{
			const ImVec2 s = ImGui::GetCurrentContext() && text ? ImGui::CalcTextSize(text) : ImVec2{};
			if (w) *w = s.x;
			if (h) *h = s.y;
		}

		MirageSetting* MIRAGE_CALL SettingFind(MirageMod* mod, const char* key)
		{
			if (!mod || !key) return nullptr;
			const auto it = mod->declared.find(key);
			return it == mod->declared.end() ? nullptr : Handle(it->second);
		}
		int32_t MIRAGE_CALL SettingType(MirageSetting* h)
		{
			ctl::Setting* s = SettingOf(h);
			if (!s) return MIRAGE_SETTING_UNKNOWN;
			if (dynamic_cast<ctl::Checkbox*>(s)) return MIRAGE_SETTING_BOOL;
			if (dynamic_cast<ctl::Combo*>(s)) return MIRAGE_SETTING_CHOICE;
			if (dynamic_cast<ctl::KeyHold*>(s)) return MIRAGE_SETTING_KEY;
			if (dynamic_cast<ctl::Value<int>*>(s)) return MIRAGE_SETTING_INT;
			if (dynamic_cast<ctl::Value<float>*>(s)) return MIRAGE_SETTING_FLOAT;
			if (dynamic_cast<ctl::Value<std::string>*>(s)) return MIRAGE_SETTING_STRING;
			return MIRAGE_SETTING_UNKNOWN;
		}

		const MirageApi kApi = {
			sizeof(MirageApi), MIRAGE_ABI_VERSION, kRuntimeVersion,
			Log, ModId, ModVersion, ModDir,
			ModuleRegister, ModuleEnabled, ModuleSetEnabled,
			SettingCheckbox, SettingSlider, SettingSliderInt, SettingText, SettingKey,
			SettingGetInt, SettingGetFloat, SettingSetInt, SettingSetFloat, SettingGetText, SettingSetText, KeyDown,
			ConfigGet, ConfigSet,
			ScreenSize, DrawText, DrawLine, DrawRect, DrawRectFilled, TextSize,
			SettingFind, SettingType,
		};

		// ---- Loading ---------------------------------------------------------------------------------------------
		std::vector<std::unique_ptr<MirageMod>> g_loaded;	// In load order
		std::vector<Info> g_found;

		void Forget(MirageMod& mod)		// Takes back the modules of a mod that failed to load
		{
			for (Module* m : mod.modules) modules::Remove(m);
			mod.modules.clear();
		}

		std::string ExceptionText(DWORD code)
		{
			char buf[64];
			std::snprintf(buf, sizeof buf, "crashed (exception 0x%08lX)", static_cast<unsigned long>(code));
			return buf;
		}

		// Loads the native part. "" when it loaded, else why not.
		std::string LoadNative(MirageMod& mod)
		{
			const auto path = mod.manifest.dir / FromUtf8(mod.manifest.entry);
			// The mod's own folder is searched for the DLLs it depends on
			mod.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (!mod.dll) return "its DLL can't be loaded (error " + std::to_string(GetLastError()) + "; is it 32-bit?)";
			const auto abi = reinterpret_cast<MirageModAbiFn>(GetProcAddress(mod.dll, "mirage_mod_abi"));
			const auto load = reinterpret_cast<MirageModLoadFn>(GetProcAddress(mod.dll, "mirage_mod_load"));
			mod.unload = reinterpret_cast<MirageModUnloadFn>(GetProcAddress(mod.dll, "mirage_mod_unload"));
			if (!abi || !load) return "its DLL doesn't export mirage_mod_abi and mirage_mod_load";
			const uint32_t version = abi();
			if (version < 1 || version > MIRAGE_ABI_VERSION)
				return "it was built for Mirage ABI " + std::to_string(version) + ", this runtime has ABI " +
					   std::to_string(MIRAGE_ABI_VERSION);
			int32_t result = 0;
			if (const DWORD code = GuardedLoad(load, &kApi, &mod, &result)) return "mirage_mod_load " + ExceptionText(code);
			if (result != 0) return "mirage_mod_load returned " + std::to_string(result);
			return {};
		}

		void Unload(MirageMod& mod)
		{
			mod.lua.reset();
			if (mod.unload)
				if (const DWORD code = GuardedUnload(mod.unload))
					spdlog::error("[mod {}] mirage_mod_unload {}", mod.id, ExceptionText(code));
			mod.unload = nullptr;
			if (mod.dll) FreeLibrary(mod.dll);
			mod.dll = nullptr;
		}
	}

	const MirageApi* Api() { return &kApi; }

	void LoadAll(const std::filesystem::path& folder)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(folder, ec)) {
			spdlog::info("Mods: no {} folder", Utf8(folder));
			return;
		}
		const auto runtime = *semver::Version::Parse(kRuntimeVersion);

		// Every manifest first: dependencies are checked against all the mods present
		std::vector<std::filesystem::path> dirs;
		for (const auto& e : std::filesystem::directory_iterator(folder, ec))
			if (e.is_directory() && std::filesystem::exists(e.path() / "mod.toml")) dirs.push_back(e.path());
		std::sort(dirs.begin(), dirs.end());
		std::vector<Manifest> manifests;
		std::map<std::string, semver::Version> present;
		for (const auto& dir : dirs) {
			Parsed p = Load(dir);
			Info info;
			info.dir = dir;
			info.id = Utf8(dir.filename());
			info.warnings = p.warnings;
			for (const auto& w : p.warnings) spdlog::warn("Mods: {}", w);
			if (!p.manifest) {
				info.problem = p.errors.empty() ? "mod.toml is invalid" : p.errors.front();
				for (const auto& e : p.errors) spdlog::error("Mods: {}", e);
				g_found.push_back(std::move(info));
				continue;
			}
			present.emplace(p.manifest->id, p.manifest->version);
			manifests.push_back(std::move(*p.manifest));
		}

		for (Manifest& m : manifests) {
			Info info;
			info.id = m.id;
			info.name = m.name;
			info.version = m.version.ToString();
			info.dir = m.dir;
			info.kind = !m.entry.empty() && !m.lua.empty() ? "native + lua" : !m.entry.empty() ? "native" : !m.lua.empty() ? "lua" : "content";
			const auto skip = [&](std::string why) {
				info.problem = std::move(why);
				spdlog::error("Mods: {} {} not loaded: {}", m.id, info.version, info.problem);
			};
			std::string problem;
			if (!m.mirage.Matches(runtime))
				problem = "it needs Mirage " + m.mirage.Text() + ", this is " + kRuntimeVersion;
			for (const Dependency& d : m.required) {
				if (!problem.empty()) break;
				const auto it = present.find(d.id);
				if (it == present.end()) problem = "it needs " + d.id + " " + d.range.Text() + ", which isn't installed";
				else if (!d.range.Matches(it->second))
					problem = "it needs " + d.id + " " + d.range.Text() + ", " + it->second.ToString() + " is installed";
			}
			for (const Dependency& d : m.incompatible) {
				if (!problem.empty()) break;
				if (const auto it = present.find(d.id); it != present.end() && d.range.Matches(it->second))
					problem = "it doesn't work with " + d.id + " " + it->second.ToString();
			}
			if (std::any_of(g_loaded.begin(), g_loaded.end(), [&](const auto& l) { return l->id == m.id; }))
				problem = "a mod with this id is already loaded";
			if (!problem.empty()) {
				skip(problem);
				g_found.push_back(std::move(info));
				continue;
			}

			auto mod = std::make_unique<MirageMod>();
			mod->manifest = m;
			mod->id = m.id;
			mod->version = info.version;
			mod->dir = Utf8(m.dir);
			if (!m.entry.empty()) problem = LoadNative(*mod);
			if (problem.empty() && !m.lua.empty()) {
				std::string error;
				mod->lua = LoadLua(&kApi, mod.get(), m.dir / FromUtf8(m.lua), error);
				if (!mod->lua) problem = m.lua + ": " + error;
			}
			if (!problem.empty()) {
				Forget(*mod);
				Unload(*mod);
				skip(problem);
				g_found.push_back(std::move(info));
				continue;
			}
			for (const SettingDecl& d : m.settings)
				if (!mod->declared.count(d.key)) {
					const std::string w = "setting " + d.key + " was not shown: " +
						(d.module.empty() ? std::string("the mod registered no module") : "it registered no module named '" + d.module + "'");
					spdlog::warn("[mod {}] {}", m.id, w);
					info.warnings.push_back(w);
				}
			info.loaded = true;
			for (Module* module : mod->modules) info.modules.push_back(module->Name());
			spdlog::info("Mods: loaded {} {} ({}, {} module(s){})", m.id, info.version, info.kind, mod->modules.size(),
						 mod->lua ? ", Lua " + std::to_string(LuaMemory(*mod->lua) / 1024) + " KB" : std::string());
			g_found.push_back(std::move(info));
			g_loaded.push_back(std::move(mod));
		}
	}

	void UnloadAll()
	{
		for (auto it = g_loaded.rbegin(); it != g_loaded.rend(); ++it) {
			Unload(**it);
			spdlog::info("Mods: unloaded {}", (*it)->id);
		}
		// The records stay: the menu's module list still holds the mods' modules (never called again) until the DLL
		// goes, and they point at their mod
	}

	std::vector<Info> List() { return g_found; }
}
