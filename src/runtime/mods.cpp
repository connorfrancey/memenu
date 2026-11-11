#include "runtime/mods.h"
#include "core/notify.h"

#include "core/module.h"
#include "core/setting.h"
#include "game/game.h"
#include "runtime/content.h"
#include "runtime/manifest.h"
#include "runtime/mod_lua.h"
#include "runtime/resolve.h"

#include <imgui.h>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <typeinfo>

#include "runtime/mod_game.h"
#include "runtime/mod_internal.h"

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

		// ---- Calling into a mod ------------------------------------------------------------------------------------
		// A crash (an access violation, a C++ exception thrown across the boundary) is caught and reported instead of
		// taking the game down. Plain functions: __try can't share a frame with C++ objects.
		struct Fault
		{
			DWORD code = 0;
			const void* address = nullptr;
		};
		// Guarded calls running on this thread: a fault inside one is the guard's, not the crash handler's
		thread_local int t_guarded = 0;

		int Catch(EXCEPTION_POINTERS* ep, Fault* f)
		{
			f->code = ep->ExceptionRecord->ExceptionCode;
			f->address = ep->ExceptionRecord->ExceptionAddress;
			return EXCEPTION_EXECUTE_HANDLER;
		}
		bool Guarded(MirageCallback cb, void* user, Fault* f)
		{
			++t_guarded;
			__try {
				cb(user);
				--t_guarded;
				return true;
			} __except (Catch(GetExceptionInformation(), f)) {
				--t_guarded;
				return false;
			}
		}
		bool GuardedLoad(MirageModLoadFn load, const MirageApi* api, MirageMod* mod, int32_t* result, Fault* f)
		{
			++t_guarded;
			__try {
				*result = load(api, mod);
				--t_guarded;
				return true;
			} __except (Catch(GetExceptionInformation(), f)) {
				--t_guarded;
				return false;
			}
		}
		bool GuardedUnload(MirageModUnloadFn unload, Fault* f)
		{
			++t_guarded;
			__try {
				unload();
				--t_guarded;
				return true;
			} __except (Catch(GetExceptionInformation(), f)) {
				--t_guarded;
				return false;
			}
		}

		// "hello.dll+0x1A2B" (the mod's own DLL by its name in mod.toml, not the hot reload copy)
		std::string Where(const MirageMod* mod, const void* address)
		{
			HMODULE m = nullptr;
			if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
												static_cast<LPCWSTR>(address), &m))
				return "an unknown address";
			std::string name;
			if (mod && m == mod->dll) {
				name = mod->manifest.entry;
			} else {
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(m, path, MAX_PATH);
				name = Utf8(std::filesystem::path(path).filename());
			}
			char off[32];
			std::snprintf(off, sizeof off, "+0x%X", static_cast<unsigned>(static_cast<const char*>(address) - reinterpret_cast<const char*>(m)));
			return name + off;
		}
		std::string Hex(DWORD code)
		{
			char buf[16];
			std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(code));
			return buf;
		}

		// ---- Crash records: <mods folder>/crashes.toml, one table per mod ------------------------------------------
		std::mutex g_crashLock;
		void RecordCrash(const MirageMod& mod, const std::string& module, const std::string& what, DWORD code,
						 const std::string& where)
		{
			std::lock_guard lock(g_crashLock);
			const auto file = mod.manifest.dir.parent_path() / "crashes.toml";
			toml::ordered_value root = toml::ordered_table{};
			try {
				if (std::filesystem::exists(file)) root = toml::parse<toml::ordered_type_config>(file);
			} catch (const std::exception&) {
				root = toml::ordered_table{};
			}
			const std::time_t now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			char when[32];
			std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &local);
			toml::ordered_value entry = toml::ordered_table{};
			entry["time"] = std::string(when);
			entry["version"] = mod.version;
			entry["module"] = module;
			entry["call"] = what;
			entry["exception"] = Hex(code);
			entry["at"] = where;
			root[mod.id] = entry;
			std::ofstream(file, std::ios::binary | std::ios::trunc)
				<< "# Mods that crashed. A mod listed here isn't loaded: delete its table to try it again.\n\n"
				<< toml::format(root);
		}
		std::map<std::string, std::string> ReadCrashes(const std::filesystem::path& folder)
		{
			std::map<std::string, std::string> off;
			const auto file = folder / "crashes.toml";
			if (!std::filesystem::exists(file)) return off;
			try {
				const auto root = toml::parse<toml::ordered_type_config>(file);
				for (const auto& [id, e] : root.as_table()) {
					const auto field = [&](const char* k) {
						return e.is_table() && e.contains(k) && e.at(k).is_string() ? e.at(k).as_string() : std::string("?");
					};
					off[id] = "it crashed the last time (" + field("call") + " of " + field("module") + ": exception " +
							  field("exception") + " at " + field("at") + "); delete its entry in " + Utf8(file) +
							  " to load it again";
				}
			} catch (const std::exception& e) {
				spdlog::warn("Mods: {} can't be read: {}", Utf8(file), e.what());
			}
			return off;
		}
	}

	// ---- A mod's module: a slot that keeps its settings and state across reloads of the mod's code ------------------
	class ModModule final : public Module
	{
	public:
		ModModule(MirageMod* mod, const MirageModuleDesc& d)
			: Module(d.name, d.tab && *d.tab ? d.tab : "Mods", Str(d.description), d.order ? d.order : 100), mod_(mod)
		{
			key = d.key;
			enabledByDefault = (d.flags & MIRAGE_MODULE_ENABLED_BY_DEFAULT) != 0;
			Bind(d);
		}

		// Its code, from the current load of the mod
		void Bind(const MirageModuleDesc& d)
		{
			desc_ = d;
			action_ = (d.flags & MIRAGE_MODULE_ACTION) != 0;
			bound_ = true;
		}
		void Unbind()
		{
			bound_ = false;
			desc_ = {};
		}
		bool Bound() const { return bound_; }

		bool IsAction() const override { return action_; }
		// A module whose mod isn't running (crashed, failed to reload, or no longer registered it) isn't listed
		bool Hidden() const override
		{
			const State s = mod_->state.load();
			return !bound_ || (s != State::Active && s != State::Loading);
		}
		void OnEnable() override { Call(desc_.on_enable, "on_enable"); }
		void OnDisable() override { Call(desc_.on_disable, "on_disable"); }
		void OnTick() override { Call(desc_.on_tick, "on_tick"); }
		void OnRender() override { Call(desc_.on_render, "on_render"); }
		void OnTrigger() override { Call(desc_.on_trigger, "on_trigger"); }

		MirageMod* Mod() const { return mod_; }
		std::vector<std::unique_ptr<ctl::Setting>> owned;	// The settings the mod made

	private:
		void Call(MirageCallback cb, const char* what);

		MirageMod* mod_;
		MirageModuleDesc desc_{};
		bool action_ = false;
		bool bound_ = false;
	};

	namespace
	{
		bool Running(const MirageMod& mod)
		{
			const State s = mod.state.load();
			return s == State::Active || s == State::Loading;
		}

		// A crash in `mod`'s code: everything of it stops, it is named, and it won't load next time
		void Crashed(MirageMod& mod, const std::string& module, const std::string& what, const Fault& f)
		{
			const std::string where = Where(&mod, f.address);
			mod.state = State::Crashed;
			mod.problem = "crashed: " + what + " of " + module + ", exception " + Hex(f.code) + " at " + where;
			spdlog::error("[mod {}] CRASHED in {} of {}: exception {} at {}. The mod is stopped (the rest of the menu "
						  "and the other mods go on) and won't be loaded next time (crashes.toml)",
						  mod.id, what, module, Hex(f.code), where);
			RecordCrash(mod, module, what, f.code, where);
		}
	}

	void ModModule::Call(MirageCallback cb, const char* what)
	{
		if (!cb || !bound_ || !Running(*mod_)) return;
		mod_->inflight.fetch_add(1);
		if (!Running(*mod_)) {		// Stopped meanwhile (a reload waits for this count)
			mod_->inflight.fetch_sub(1);
			return;
		}
		Fault f;
		const bool ok = Guarded(cb, desc_.user, &f);
		mod_->inflight.fetch_sub(1);
		if (ok) return;
		Crashed(*mod_, Name(), what, f);
		for (ModModule* m : mod_->modules)		// Off without calling it again (its code is in an unknown state)
			if (m->Enabled()) m->SetEnabled(false);
		detail::RemoveGameHooks(*mod_);
	}

	// A content mod's switch: its overrides on (applied on the game thread) or off (put back as they were)
	class ContentModule final : public Module
	{
	public:
		ContentModule(MirageMod* mod, const std::string& name)
			: Module(name, "Mods", mod->manifest.description.empty() ? "What this mod changes in the game" : mod->manifest.description, 50),
			  mod_(mod)
		{
			enabledByDefault = true;
		}
		bool Hidden() const override
		{
			const State st = mod_->state.load();
			return st != State::Active && st != State::Loading;
		}
		void OnEnable() override
		{
			auto set = mod_->content;
			if (set) game::RunOnGameThread([set] { content::Apply(*set); });
		}
		void OnDisable() override
		{
			auto set = mod_->content;
			if (set) game::RunOnGameThread([set] { if (content::Applied(*set)) content::Restore(*set); });
		}

	private:
		MirageMod* mod_;
	};

	bool detail::Running(const MirageMod& mod)
	{
		const State st = mod.state.load();
		return st == State::Active || st == State::Loading;
	}

	bool detail::Enter(MirageMod& mod, const std::string& what, const std::string& where, void (*fn)(void*), void* arg)
	{
		if (!Running(mod)) return false;
		mod.inflight.fetch_add(1);
		if (!Running(mod)) {
			mod.inflight.fetch_sub(1);
			return false;
		}
		Fault f;
		const bool ok = Guarded(fn, arg, &f);
		mod.inflight.fetch_sub(1);
		if (ok) return true;
		Crashed(mod, where, what, f);
		for (ModModule* m : mod.modules)
			if (m->Enabled()) m->SetEnabled(false);
		RemoveGameHooks(mod);
		return false;
	}

	namespace
	{
		ctl::Setting* SettingOf(MirageSetting* s) { return reinterpret_cast<ctl::Setting*>(s); }
		MirageSetting* Handle(ctl::Setting* s) { return reinterpret_cast<MirageSetting*>(s); }
		ModModule* ModuleOf(MirageModule* m) { return reinterpret_cast<ModModule*>(m); }

		// A setting made in code. On a reload the module has it already: the same name and kind gives it back, value
		// and all.
		template <class T, class... Args>
		MirageSetting* MakeSetting(MirageModule* module, const char* name, Args&&... args)
		{
			ModModule* m = ModuleOf(module);
			if (!m || !name || !*name) return nullptr;
			if (ctl::Setting* existing = m->FindSetting(name)) {
				if (typeid(*existing) == typeid(T)) return Handle(existing);
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
		// this is the mod's first module), in the file's order. On a reload the ones made before are kept.
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
						// Within ImGui's int slider range (see SettingSliderInt)
						s = std::make_unique<ctl::SliderInt>(&m, d.name, static_cast<int>(d.intDefault),
															 static_cast<int>(std::clamp<double>(d.min, -(INT32_MAX / 2), INT32_MAX / 2)),
															 static_cast<int>(std::clamp<double>(d.max, -(INT32_MAX / 2), INT32_MAX / 2)),
															 std::string("%d"), 0, d.description);
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
			if (mod->state.load() != State::Loading) {
				spdlog::error("[mod {}] module_register: modules are registered while the mod loads", mod->id);
				return nullptr;
			}
			// Only the part of the description the mod's header had; the rest stays zero
			MirageModuleDesc d{};
			std::memcpy(&d, desc, std::min<std::size_t>(desc->size, sizeof d));
			d.size = sizeof d;
			if (desc->size < offsetof(MirageModuleDesc, user) || !d.name || !*d.name) {
				spdlog::error("[mod {}] module_register: a module needs a name", mod->id);
				return nullptr;
			}
			// A module this mod registered in an earlier load: the same slot, its settings and state kept
			for (ModModule* m : mod->modules)
				if (m->Name() == d.name) {
					if (m->Bound()) {
						spdlog::error("[mod {}] module_register: '{}' is registered twice", mod->id, d.name);
						return nullptr;
					}
					m->Bind(d);
					AddDeclaredSettings(*mod, *m, m == mod->modules.front());	// New keys in mod.toml
					return reinterpret_cast<MirageModule*>(m);
				}
			if (modules::NameTaken(d.name)) {
				spdlog::error("[mod {}] module_register: a module named '{}' already exists", mod->id, d.name);
				return nullptr;
			}
			auto owned = std::make_unique<ModModule>(mod, d);
			ModModule* m = owned.get();
			const bool first = mod->modules.empty();
			mod->modules.push_back(m);
			AddDeclaredSettings(*mod, *m, first);
			mod->pending.push_back(std::move(owned));	// Joins the menu when the load is done
			return reinterpret_cast<MirageModule*>(m);
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
			// ImGui's int sliders take at most half of int32's range (it asserts otherwise): 1 << 30 is one too many
			constexpr int32_t kLimit = INT32_MAX / 2;
			min = std::clamp(min, -kLimit, kLimit);
			max = std::clamp(max, -kLimit, kLimit);
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

		// A notification on screen (the menu's toasts), prefixed with nothing: the mod writes what the player reads
		void MIRAGE_CALL Notify(MirageMod* mod, const char* text)
		{
			if (!mod || !text || !*text) return;
			notify::Push(text);
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
			game_api::EventSubscribe, game_api::EventUnsubscribe, game_api::HookFunction, game_api::HookRemove,
			game_api::CallObject, game_api::CallFunction, game_api::CallSkip, game_api::CallGet, game_api::CallSet,
			game_api::CallBegin, game_api::CallInvoke, game_api::CallEnd,
			game_api::PlayerPawn, game_api::PlayerController, game_api::WorldInfo, game_api::ObjectFind,
			game_api::ObjectGet, game_api::ObjectSet, game_api::ObjectName, game_api::ObjectClassName, game_api::ObjectIsA,
			game_api::DetourCreate, game_api::DetourRemove, game_api::FindPattern,
			game_api::ScriptOpen, game_api::ScriptClose, game_api::ScriptCount, game_api::ScriptGet, game_api::ScriptSet,
			game_api::ScriptAdd, game_api::ScriptChildren, game_api::ScriptSetChildren, game_api::ScriptApply,
			game_api::ScriptRestore, game_api::ScriptDump, game_api::ScriptKind, game_api::ScriptKindName,
			Notify,
		};

		// ---- Loading -----------------------------------------------------------------------------------------------
		std::vector<std::unique_ptr<MirageMod>> g_mods;		// Discovered and to be loaded, in load order
		std::vector<Info> g_excluded;						// Discovered and left out
		int g_loadCount = 0;
		std::atomic<int> g_shadowCounter{0};

		// The files of a mod, summed up: changes when one is added, removed, rewritten or touched
		std::uintmax_t Fingerprint(const std::filesystem::path& dir)
		{
			std::uintmax_t h = 1469598103934665603ull;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
				 it.increment(ec)) {
				const auto& p = it->path();
				if (it->is_directory()) {
					if (p.filename() == ".mirage") it.disable_recursion_pending();	// Its hot reload copies
					continue;
				}
				if (p.filename() == "config.toml") continue;	// Written by the mod itself
				const auto mix = [&](std::uintmax_t v) { h = (h ^ v) * 1099511628211ull; };
				mix(std::hash<std::wstring>{}(p.wstring()));
				mix(static_cast<std::uintmax_t>(it->last_write_time(ec).time_since_epoch().count()));
				mix(it->file_size(ec));
			}
			return h;
		}

		void UnloadCode(MirageMod& mod)
		{
			detail::RemoveGameHooks(mod);	// Before its code goes
			if (mod.content && content::Applied(*mod.content)) content::Restore(*mod.content);
			mod.lua.reset();
			if (mod.unload) {
				Fault f;
				if (!GuardedUnload(mod.unload, &f))
					spdlog::error("[mod {}] mirage_mod_unload crashed: exception {} at {}", mod.id, Hex(f.code), Where(&mod, f.address));
			}
			mod.unload = nullptr;
			if (mod.dll) FreeLibrary(mod.dll);
			mod.dll = nullptr;
			if (!mod.shadow.empty()) {
				std::error_code ec;
				std::filesystem::remove(mod.shadow, ec);
				mod.shadow.clear();
			}
		}

		// The native part. "" when it loaded, else why not.
		std::string LoadNative(MirageMod& mod)
		{
			auto path = mod.manifest.dir / FromUtf8(mod.manifest.entry);
			if (mod.manifest.hotReload) {
				// Run a copy, so a new build can replace the DLL while the game runs
				const auto folder = mod.manifest.dir / ".mirage";
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				for (const auto& old : std::filesystem::directory_iterator(folder, ec))
					std::filesystem::remove(old.path(), ec);	// Left by a crash; skipped when still loaded
				mod.shadow = folder / (path.stem().wstring() + L"-" + std::to_wstring(++g_shadowCounter) + L".dll");
				if (!std::filesystem::copy_file(path, mod.shadow, std::filesystem::copy_options::overwrite_existing, ec))
					return "its DLL can't be copied for hot reload (" + ec.message() + ")";
				path = mod.shadow;
			}
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
			Fault f;
			if (!GuardedLoad(load, &kApi, &mod, &result, &f)) {
				Crashed(mod, "(loading)", "mirage_mod_load", f);
				return mod.problem;
			}
			if (result != 0) return "mirage_mod_load returned " + std::to_string(result);
			return {};
		}

		// Loads (or reloads) a mod's code. "" when it loaded, else why not (then nothing of it runs).
		std::string LoadCode(MirageMod& mod)
		{
			mod.state = State::Loading;
			mod.pending.clear();
			for (ModModule* m : mod.modules) m->Unbind();
			mod.fingerprint = Fingerprint(mod.manifest.dir);
			std::string problem;
			if (!mod.manifest.entry.empty()) problem = LoadNative(mod);
			if (problem.empty() && !mod.manifest.lua.empty()) {
				std::string error;
				mod.lua = LoadLua(&kApi, &mod, mod.manifest.dir / FromUtf8(mod.manifest.lua), error);
				if (!mod.lua) problem = mod.manifest.lua + ": " + error;
			}
			if (!problem.empty()) {
				const bool crashed = mod.state.load() == State::Crashed;
				if (!crashed) mod.state = State::Failed;
				for (ModModule* m : mod.modules) m->Unbind();
				UnloadCode(mod);
				// Modules new in this load never reach the menu
				for (const auto& p : mod.pending) {
					std::erase(mod.modules, static_cast<ModModule*>(p.get()));
					std::erase_if(mod.declared, [&](const auto& kv) { return &kv.second->Owner() == p.get(); });
				}
				mod.pending.clear();
				mod.problem = problem;
				return problem;
			}
			// Its content: read again on every load (a reload picks up edited files)
			if (!mod.manifest.content.empty()) {
				std::vector<std::filesystem::path> folders;
				for (const auto& c : mod.manifest.content) folders.push_back(mod.manifest.dir / FromUtf8(c));
				std::vector<std::string> problems;
				auto set = content::Load(mod.id, folders, problems);
				for (const auto& w : problems) {
					spdlog::warn("[mod {}] content: {}", mod.id, w);
					mod.warnings.push_back("content: " + w);
				}
				const bool on = mod.contentModule && mod.contentModule->Enabled();
				if (mod.content) {
					if (content::Applied(*mod.content)) content::Restore(*mod.content);
					content::Unregister(*mod.content);
				}
				mod.content = content::Empty(*set) ? nullptr : set;
				if (mod.content) {
					content::Register(mod.content);
					if (!mod.contentModule) {
						std::string name = mod.manifest.name;
						if (modules::NameTaken(name) || std::any_of(mod.pending.begin(), mod.pending.end(), [&](const auto& m) { return m->Name() == name; }))
							name += " (content)";
						auto module = std::make_unique<ContentModule>(&mod, name);
						mod.contentModule = module.get();
						mod.pending.push_back(std::move(module));
					} else if (on) {
						auto applied = mod.content;
						game::RunOnGameThread([applied] { content::Apply(*applied); });
					}
				}
			}
			for (const SettingDecl& d : mod.manifest.settings)
				if (!mod.declared.count(d.key)) {
					const std::string w = "setting " + d.key + " isn't shown: " +
						(d.module.empty() ? std::string("the mod registered no module") : "it registered no module named '" + d.module + "'");
					spdlog::warn("[mod {}] {}", mod.id, w);
					mod.warnings.push_back(w);
				}
			modules::Publish(std::move(mod.pending));
			mod.pending.clear();
			mod.problem.clear();
			mod.state = State::Active;
			return {};
		}

		std::string Kind(const Manifest& m)
		{
			return !m.entry.empty() && !m.lua.empty() ? "native + lua" : !m.entry.empty() ? "native" : !m.lua.empty() ? "lua" : "content";
		}
	}

	const MirageApi* Api() { return &kApi; }

	std::map<std::string, std::string> CrashedMods(const std::filesystem::path& folder) { return ReadCrashes(folder); }

	void Discover(const std::vector<std::filesystem::path>& folders)
	{
		std::vector<Manifest> manifests;
		std::map<std::string, std::string> off;
		for (const auto& folder : folders) {
			std::error_code ec;
			if (!std::filesystem::is_directory(folder, ec)) {
				spdlog::info("Mods: no {} folder", Utf8(folder));
				continue;
			}
			off.merge(ReadCrashes(folder));
			std::vector<std::filesystem::path> dirs;
			for (const auto& e : std::filesystem::directory_iterator(folder, ec))
				if (e.is_directory() && std::filesystem::exists(e.path() / "mod.toml")) dirs.push_back(e.path());
			std::sort(dirs.begin(), dirs.end());
			for (const auto& dir : dirs) {
				Parsed p = Load(dir);
				for (const auto& w : p.warnings) spdlog::warn("Mods: {}", w);
				if (!p.manifest) {
					Info info;
					info.dir = dir;
					info.id = Utf8(dir.filename());
					info.warnings = p.warnings;
					info.problem = p.errors.empty() ? "mod.toml is invalid" : p.errors.front();
					for (const auto& e : p.errors) spdlog::error("Mods: {}", e);
					g_excluded.push_back(std::move(info));
					continue;
				}
				manifests.push_back(std::move(*p.manifest));
			}
		}

		const Resolution r = Resolve(manifests, *semver::Version::Parse(kRuntimeVersion), off);
		for (const auto& w : r.warnings) spdlog::warn("Mods: {}", w);
		for (const Manifest& m : manifests) {
			const auto it = r.excluded.find(m.id);
			if (it == r.excluded.end()) continue;
			Info info;
			info.id = m.id;
			info.name = m.name;
			info.version = m.version.ToString();
			info.dir = m.dir;
			info.phase = m.phase;
			info.kind = Kind(m);
			info.problem = it->second;
			spdlog::error("Mods: {} {} is not loaded: {}", m.id, info.version, info.problem);
			g_excluded.push_back(std::move(info));
		}
		for (const std::string& id : r.order) {
			const auto m = std::find_if(manifests.begin(), manifests.end(), [&](const Manifest& x) { return x.id == id; });
			auto mod = std::make_unique<MirageMod>();
			mod->manifest = *m;
			mod->id = m->id;
			mod->version = m->version.ToString();
			mod->dir = Utf8(m->dir);
			g_mods.push_back(std::move(mod));
		}
		if (!r.order.empty()) {
			std::string order;
			for (const auto& id : r.order) order += (order.empty() ? "" : ", ") + id;
			spdlog::info("Mods: load order {}", order);
		}
	}

	void LoadPhase(Phase phase)
	{
		for (auto& owned : g_mods) {
			MirageMod& mod = *owned;
			if (mod.manifest.phase != phase || mod.state.load() != State::NotLoaded) continue;
			// A required mod that failed to load (or crashed) takes this one with it
			std::string problem;
			for (const Dependency& d : mod.manifest.required) {
				const auto it = std::find_if(g_mods.begin(), g_mods.end(), [&](const auto& x) { return x->id == d.id; });
				if (it == g_mods.end() || (*it)->state.load() != State::Active) {
					problem = "it needs " + d.id + ", which didn't load";
					break;
				}
			}
			if (!problem.empty()) {
				mod.state = State::Failed;
				mod.problem = problem;
				spdlog::error("Mods: {} {} is not loaded: {}", mod.id, mod.version, problem);
				continue;
			}
			problem = LoadCode(mod);
			if (!problem.empty()) {
				spdlog::error("Mods: {} {} is not loaded: {}", mod.id, mod.version, problem);
				continue;
			}
			mod.loadIndex = g_loadCount++;
			spdlog::info("Mods: loaded {} {} ({} phase, {}, {} module(s){})", mod.id, mod.version, PhaseName(phase),
						 Kind(mod.manifest), mod.modules.size(),
						 mod.lua ? ", Lua " + std::to_string(LuaMemory(*mod.lua) / 1024) + " KB" : std::string());
		}
	}

	std::string Reload(const std::string& id)
	{
		const auto it = std::find_if(g_mods.begin(), g_mods.end(), [&](const auto& m) { return m->id == id; });
		if (it == g_mods.end()) return "no loaded mod has the id " + id;
		MirageMod& mod = **it;
		const State state = mod.state.load();
		if (state == State::Crashed) return "it crashed: restart the game to load it again";
		if (state != State::Active && state != State::Failed) return "it isn't loaded";
		if (!mod.manifest.hotReload) return "it doesn't support hot reload (hot_reload = true in mod.toml)";

		Parsed p = Load(mod.manifest.dir);
		if (!p.manifest) return "mod.toml: " + (p.errors.empty() ? std::string("invalid") : p.errors.front());
		if (p.manifest->id != mod.id) return "mod.toml: the id changed";

		// The old code undoes its changes, then stops
		std::vector<ModModule*> wasOn;
		for (ModModule* m : mod.modules)
			if (m->Enabled()) {
				wasOn.push_back(m);
				m->SetEnabled(false);
			}
		mod.state = State::Detached;
		const auto start = std::chrono::steady_clock::now();
		while (mod.inflight.load() > 0) {		// A call on the render thread still running
			if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
				spdlog::error("[mod {}] hot reload: a call into it didn't return in 2 s; reload skipped", mod.id);
				mod.state = State::Active;
				for (ModModule* m : wasOn) m->SetEnabled(true);
				return "a call into it didn't return";
			}
			Sleep(0);
		}
		UnloadCode(mod);

		mod.manifest = std::move(*p.manifest);
		mod.version = mod.manifest.version.ToString();
		const std::string problem = LoadCode(mod);
		++mod.reloads;
		if (!problem.empty()) {
			spdlog::error("[mod {}] hot reload failed: {} (its modules are off until it loads again)", mod.id, problem);
			return problem;
		}
		for (ModModule* m : wasOn)
			if (m->Bound()) m->SetEnabled(true);
		spdlog::info("[mod {}] reloaded ({} time(s))", mod.id, mod.reloads);
		return {};
	}

	void ReloadChanged()
	{
		for (auto& owned : g_mods) {
			MirageMod& mod = *owned;
			const State s = mod.state.load();
			if (s != State::Active && s != State::Failed) continue;
			if (mod.loadIndex < 0) continue;	// Never loaded (left out at its phase)
			const std::uintmax_t now = Fingerprint(mod.manifest.dir);
			if (now == mod.fingerprint) continue;
			if (now != mod.seen) {	// Still being written (an editor saving, a build): the next check decides
				mod.seen = now;
				continue;
			}
			if (!mod.manifest.hotReload) {
				if (!mod.warnedNoReload)
					spdlog::info("[mod {}] its files changed: restart the game to load them (or hot_reload = true)", mod.id);
				mod.warnedNoReload = true;
				mod.fingerprint = now;
				continue;
			}
			Reload(mod.id);		// Sets the fingerprint again, failed or not
		}
	}

	std::string AttributeCrash(const void* address, unsigned long code)
	{
		if (t_guarded > 0) return {};	// The guard around the call catches it (and names the mod)
		HMODULE m = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								static_cast<LPCWSTR>(address), &m))
			return {};
		for (auto& owned : g_mods)
			if (owned->dll && owned->dll == m) {
				Fault f;
				f.code = code;
				f.address = address;
				Crashed(*owned, "(its own thread)", "code outside a callback", f);
				return owned->id;
			}
		return {};
	}

	void UnloadAll()
	{
		std::vector<MirageMod*> loaded;
		for (auto& m : g_mods)
			if (m->loadIndex >= 0) loaded.push_back(m.get());
		std::sort(loaded.begin(), loaded.end(), [](const MirageMod* a, const MirageMod* b) { return a->loadIndex > b->loadIndex; });
		for (MirageMod* mod : loaded) {
			if (mod->state.load() == State::Active) mod->state = State::Detached;
			UnloadCode(*mod);
			spdlog::info("Mods: unloaded {}", mod->id);
		}
		// The records stay: the menu's module list still holds the mods' modules (never called again) until the DLL
		// goes, and they point at their mod
	}

	std::vector<Info> List()
	{
		std::vector<Info> out;
		for (const auto& owned : g_mods) {
			const MirageMod& mod = *owned;
			Info info;
			info.id = mod.id;
			info.name = mod.manifest.name;
			info.version = mod.version;
			info.dir = mod.manifest.dir;
			info.phase = mod.manifest.phase;
			info.kind = Kind(mod.manifest);
			info.loaded = mod.state.load() == State::Active;
			info.loadIndex = mod.loadIndex;
			info.reloads = mod.reloads;
			info.problem = mod.state.load() == State::NotLoaded ? std::string("not loaded yet (") + PhaseName(mod.manifest.phase) + " phase)"
															   : mod.problem;
			for (const ModModule* m : mod.modules)
				if (m->Bound()) info.modules.push_back(m->Name());
			info.warnings = mod.warnings;
			out.push_back(std::move(info));
		}
		std::stable_sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
			const auto rank = [](const Info& i) { return i.loadIndex < 0 ? 1 << 30 : i.loadIndex; };
			return rank(a) < rank(b);
		});
		out.insert(out.end(), g_excluded.begin(), g_excluded.end());
		return out;
	}
}
