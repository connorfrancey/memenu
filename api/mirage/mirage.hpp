#pragma once
// Mirage mod API for C++: a header-only wrapper over mirage/api.h, in the style of the menu's own modules.
//
//     #include <mirage/mirage.hpp>
//
//     class Hello final : public mirage::Module {
//     public:
//         Hello() : Module("Hello", "Mods", "Says hello") {}
//         void OnRender() override { mirage::DrawText(20, 200, 0xFFFFFFFF, greeting.Get()); }
//     private:
//         mirage::Text greeting{this, "Greeting", "Hello from a mod"};
//         mirage::Slider size{this, "Size", 1.f, 0.5f, 3.f};
//     };
//     MIRAGE_MODULE(Hello)
//
//     MIRAGE_MOD()    // Once per mod, in one .cpp: the exports the runtime looks for
//
// Everything here is compiled into the mod: only the C types of api.h cross into the runtime, so the mod can use any
// compiler and standard library.

#include "mirage/api.h"

#include <functional>
#include <memory>
#include <type_traits>
#include <string>
#include <vector>

namespace mirage
{
	namespace detail
	{
		inline const MirageApi*& Api()
		{
			static const MirageApi* api = nullptr;
			return api;
		}
		inline MirageMod*& Mod()
		{
			static MirageMod* mod = nullptr;
			return mod;
		}
	}

	inline const MirageApi& Api() { return *detail::Api(); }
	inline MirageMod* Mod() { return detail::Mod(); }

	// ---- Logging ---------------------------------------------------------------------------------------------------
	inline void Log(MirageLogLevel level, const std::string& text) { Api().log(Mod(), level, text.c_str()); }
	inline void Info(const std::string& text) { Log(MIRAGE_LOG_INFO, text); }
	inline void Warn(const std::string& text) { Log(MIRAGE_LOG_WARN, text); }
	inline void Error(const std::string& text) { Log(MIRAGE_LOG_ERROR, text); }

	inline std::string Id() { return Api().mod_id(Mod()); }
	inline std::string Dir() { return Api().mod_dir(Mod()); }

	// ---- Per-mod config (<mod folder>/config.toml) -----------------------------------------------------------------
	inline std::string Config(const std::string& key, const std::string& fallback = {})
	{
		char small[256];
		const uint32_t n = Api().config_get(Mod(), key.c_str(), fallback.c_str(), small, sizeof small);
		if (n < sizeof small) return std::string(small, n);
		std::string big(n + 1, '\0');
		Api().config_get(Mod(), key.c_str(), fallback.c_str(), big.data(), n + 1);
		big.resize(n);
		return big;
	}
	inline void SetConfig(const std::string& key, const std::string& value)
	{
		Api().config_set(Mod(), key.c_str(), value.c_str());
	}

	// ---- Settings declared in mod.toml ([settings.<key>]), by key ----------------------------------------------------
	// They exist once the module they belong to is registered (after its mirage::Module constructor).
	//     const float height = mirage::Get<float>("height");
	//     mirage::Set("greeting", "Hi");
	inline MirageSetting* FindSetting(const std::string& key) { return Api().setting_find(Mod(), key.c_str()); }
	template <class T>
	T Get(const std::string& key)
	{
		MirageSetting* s = FindSetting(key);
		if constexpr (std::is_same_v<T, bool>) {
			return s && Api().setting_get_int(s) != 0;
		} else if constexpr (std::is_integral_v<T>) {
			return s ? static_cast<T>(Api().setting_get_int(s)) : T{};
		} else if constexpr (std::is_floating_point_v<T>) {
			return s ? static_cast<T>(Api().setting_get_float(s)) : T{};
		} else {
			static_assert(std::is_same_v<T, std::string>, "Get<bool / int / float / std::string>");
			if (!s) return {};
			const uint32_t n = Api().setting_get_text(s, nullptr, 0);
			std::string text(n + 1, '\0');
			Api().setting_get_text(s, text.data(), n + 1);
			text.resize(n);
			return text;
		}
	}
	inline void Set(const std::string& key, bool v) { if (auto* s = FindSetting(key)) Api().setting_set_int(s, v ? 1 : 0); }
	inline void Set(const std::string& key, int v) { if (auto* s = FindSetting(key)) Api().setting_set_int(s, v); }
	inline void Set(const std::string& key, float v) { if (auto* s = FindSetting(key)) Api().setting_set_float(s, v); }
	inline void Set(const std::string& key, const std::string& v)
	{
		if (auto* s = FindSetting(key)) Api().setting_set_text(s, v.c_str());
	}
	inline void Set(const std::string& key, const char* v) { Set(key, std::string(v)); }

	// ---- Drawing (OnRender only) ----------------------------------------------------------------------------------
	inline void ScreenSize(float& w, float& h) { Api().screen_size(&w, &h); }
	inline void DrawText(float x, float y, uint32_t color, const std::string& text)
	{
		Api().draw_text(x, y, color, text.c_str());
	}
	inline void DrawLine(float x1, float y1, float x2, float y2, uint32_t color, float thickness = 1.f)
	{
		Api().draw_line(x1, y1, x2, y2, color, thickness);
	}
	inline void DrawRect(float x1, float y1, float x2, float y2, uint32_t color, float thickness = 1.f)
	{
		Api().draw_rect(x1, y1, x2, y2, color, thickness);
	}
	inline void DrawRectFilled(float x1, float y1, float x2, float y2, uint32_t color)
	{
		Api().draw_rect_filled(x1, y1, x2, y2, color);
	}
	// IM_COL32 order: 0xAABBGGRR
	constexpr uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
	{
		return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	}

	// ---- Modules ---------------------------------------------------------------------------------------------------
	class Module
	{
	public:
		// `key`: default toggle key (virtual-key code); `flags`: MIRAGE_MODULE_ACTION, MIRAGE_MODULE_ENABLED_BY_DEFAULT
		Module(const std::string& name, const std::string& tab = "Mods", const std::string& description = {},
			   int order = 100, int key = 0, uint32_t flags = 0)
		{
			MirageModuleDesc d{};
			d.size = sizeof d;
			d.name = name.c_str();
			d.tab = tab.c_str();
			d.description = description.c_str();
			d.order = order;
			d.key = key;
			d.flags = flags;
			d.user = this;
			d.on_enable = [](void* u) { static_cast<Module*>(u)->OnEnable(); };
			d.on_disable = [](void* u) { static_cast<Module*>(u)->OnDisable(); };
			d.on_tick = [](void* u) { static_cast<Module*>(u)->OnTick(); };
			d.on_render = [](void* u) { static_cast<Module*>(u)->OnRender(); };
			d.on_trigger = [](void* u) { static_cast<Module*>(u)->OnTrigger(); };
			handle_ = Api().module_register(Mod(), &d);
		}
		virtual ~Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		virtual void OnEnable() {}
		virtual void OnDisable() {}
		virtual void OnTick() {}
		virtual void OnRender() {}
		virtual void OnTrigger() {}

		bool Enabled() const { return handle_ && Api().module_enabled(handle_) != 0; }
		void SetEnabled(bool on) { if (handle_) Api().module_set_enabled(handle_, on ? 1 : 0); }
		bool Registered() const { return handle_ != nullptr; }
		MirageModule* Handle() const { return handle_; }

	private:
		MirageModule* handle_ = nullptr;
	};

	// ---- Settings --------------------------------------------------------------------------------------------------
	class Setting
	{
	public:
		MirageSetting* Handle() const { return handle_; }
		bool Valid() const { return handle_ != nullptr; }

	protected:
		MirageSetting* handle_ = nullptr;
	};

	class Checkbox : public Setting
	{
	public:
		Checkbox(Module* owner, const std::string& name, bool value = false, const std::string& tooltip = {})
		{
			handle_ = Api().setting_checkbox(owner->Handle(), name.c_str(), value ? 1 : 0, tooltip.c_str());
		}
		bool Get() const { return handle_ && Api().setting_get_int(handle_) != 0; }
		operator bool() const { return Get(); }
		void Set(bool v) { if (handle_) Api().setting_set_int(handle_, v ? 1 : 0); }
	};

	class Slider : public Setting
	{
	public:
		Slider(Module* owner, const std::string& name, float value, float min, float max,
			   const std::string& format = "%.2f", const std::string& tooltip = {})
		{
			handle_ = Api().setting_slider(owner->Handle(), name.c_str(), value, min, max, format.c_str(),
										   tooltip.c_str());
		}
		float Get() const { return handle_ ? Api().setting_get_float(handle_) : 0.f; }
		operator float() const { return Get(); }
		void Set(float v) { if (handle_) Api().setting_set_float(handle_, v); }
	};

	class SliderInt : public Setting
	{
	public:
		SliderInt(Module* owner, const std::string& name, int value, int min, int max, const std::string& tooltip = {})
		{
			handle_ = Api().setting_slider_int(owner->Handle(), name.c_str(), value, min, max, tooltip.c_str());
		}
		int Get() const { return handle_ ? Api().setting_get_int(handle_) : 0; }
		operator int() const { return Get(); }
		void Set(int v) { if (handle_) Api().setting_set_int(handle_, v); }
	};

	class Text : public Setting
	{
	public:
		Text(Module* owner, const std::string& name, const std::string& value = {}, const std::string& tooltip = {})
		{
			handle_ = Api().setting_text(owner->Handle(), name.c_str(), value.c_str(), tooltip.c_str());
		}
		std::string Get() const
		{
			if (!handle_) return {};
			const uint32_t n = Api().setting_get_text(handle_, nullptr, 0);
			std::string s(n + 1, '\0');
			Api().setting_get_text(handle_, s.data(), n + 1);
			s.resize(n);
			return s;
		}
		void Set(const std::string& v) { if (handle_) Api().setting_set_text(handle_, v.c_str()); }
	};

	class KeyHold : public Setting
	{
	public:
		KeyHold(Module* owner, const std::string& name, int vk = 0, const std::string& tooltip = {})
		{
			handle_ = Api().setting_key(owner->Handle(), name.c_str(), vk, tooltip.c_str());
		}
		bool Down() const { return handle_ && Api().key_down(handle_) != 0; }
		int Key() const { return handle_ ? Api().setting_get_int(handle_) : 0; }
	};

	// ---- Game values (step 6): what object_get / call_get return, as C++ types ---------------------------------------
	class Object;
	namespace detail
	{
		template <class T>
		T FromValue(const MirageValue& v);
		template <class T>
		MirageValue ToValue(const T& x)
		{
			MirageValue v{};
			if constexpr (std::is_same_v<T, bool>) {
				v.type = MIRAGE_VALUE_BOOL;
				v.i = x ? 1 : 0;
			} else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
				v.type = MIRAGE_VALUE_INT;
				v.i = static_cast<int32_t>(x);
			} else if constexpr (std::is_floating_point_v<T>) {
				v.type = MIRAGE_VALUE_FLOAT;
				v.f = static_cast<float>(x);
			} else if constexpr (std::is_same_v<T, std::string>) {
				v.type = MIRAGE_VALUE_TEXT;
				const std::size_t n = x.size() < sizeof v.text - 1 ? x.size() : sizeof v.text - 1;
				for (std::size_t i = 0; i < n; ++i) v.text[i] = x[i];
			} else {
				static_assert(std::is_same_v<T, Object>, "a bool, number, std::string or mirage::Object");
				v.type = MIRAGE_VALUE_OBJECT;
				v.o = x.Handle();
			}
			return v;
		}
	}

	// A game object: an Actor, a Controller, ... Not owned: look it up again rather than keep it across ticks.
	//     mirage::Object pawn = mirage::Object::Pawn();
	//     if (pawn) pawn.Set("Location.Z", pawn.Get<float>("Location.Z") + 100.f);
	class Object
	{
	public:
		Object() = default;
		explicit Object(MirageObject* o) : o_(o) {}
		static Object Pawn() { return Object(Api().player_pawn()); }
		static Object Controller() { return Object(Api().player_controller()); }
		static Object World() { return Object(Api().world_info()); }
		static Object Find(const std::string& fullName) { return Object(Api().object_find(fullName.c_str())); }

		explicit operator bool() const { return o_ != nullptr; }
		bool operator==(const Object& other) const { return o_ == other.o_; }
		MirageObject* Handle() const { return o_; }

		// A property, or a member of a struct in it ("Location.Z"): T is bool, a number, std::string or Object.
		// Missing or of another kind: T{}.
		template <class T>
		T Get(const std::string& path) const
		{
			MirageValue v{};
			if (!o_ || !Api().object_get(o_, path.c_str(), &v)) return T{};
			return detail::FromValue<T>(v);
		}
		template <class T>
		bool Set(const std::string& path, const T& x) const
		{
			const MirageValue v = detail::ToValue(x);
			return o_ && Api().object_set(o_, path.c_str(), &v) != 0;
		}
		std::string Name() const { return Text(Api().object_name); }
		std::string ClassName() const { return Text(Api().object_class_name); }
		bool IsA(const std::string& className) const { return o_ && Api().object_is_a(o_, className.c_str()) != 0; }

	private:
		std::string Text(uint32_t(MIRAGE_CALL* fn)(MirageObject*, char*, uint32_t)) const
		{
			char buffer[256];
			if (!o_) return {};
			fn(o_, buffer, sizeof buffer);
			return buffer;
		}
		MirageObject* o_ = nullptr;
	};

	namespace detail
	{
		template <class T>
		T FromValue(const MirageValue& v)
		{
			if constexpr (std::is_same_v<T, bool>) {
				return v.i != 0;
			} else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
				return static_cast<T>(v.type == MIRAGE_VALUE_FLOAT ? static_cast<int32_t>(v.f) : v.i);
			} else if constexpr (std::is_floating_point_v<T>) {
				return static_cast<T>(v.type == MIRAGE_VALUE_FLOAT ? v.f : static_cast<float>(v.i));
			} else if constexpr (std::is_same_v<T, std::string>) {
				return std::string(v.text);
			} else {
				static_assert(std::is_same_v<T, Object>, "a bool, number, std::string or mirage::Object");
				return Object(v.o);
			}
		}
	}

	// A call of a game function, in a hook: its object, its parameters ("NewLocation.Z", "ReturnValue")
	class Call
	{
	public:
		explicit Call(MirageCall* c) : c_(c) {}
		Object Self() const { return Object(Api().call_object(c_)); }
		std::string Function() const { return Api().call_function(c_); }
		void Skip() const { Api().call_skip(c_); }
		template <class T>
		T Get(const std::string& path) const
		{
			MirageValue v{};
			if (!Api().call_get(c_, path.c_str(), &v)) return T{};
			return detail::FromValue<T>(v);
		}
		template <class T>
		bool Set(const std::string& path, const T& x) const
		{
			const MirageValue v = detail::ToValue(x);
			return Api().call_set(c_, path.c_str(), &v) != 0;
		}

	private:
		MirageCall* c_;
	};

	// ---- Events and hooks (step 6). Removed when the mod unloads; Remove() takes one off earlier. --------------------
	class Hooked
	{
	public:
		virtual ~Hooked() = default;
		virtual void Remove() = 0;
	};
	namespace detail
	{
		inline std::vector<std::unique_ptr<Hooked>>& Callbacks()
		{
			static std::vector<std::unique_ptr<Hooked>> c;
			return c;
		}
		struct EventCallback final : Hooked
		{
			std::function<void(const MirageEvent&)> fn;
			MirageSubscription* sub = nullptr;
			void Remove() override
			{
				if (sub) ::mirage::Api().event_unsubscribe(sub);
				sub = nullptr;
			}
		};
		struct HookCallback final : Hooked
		{
			std::function<void(Call&)> fn;
			MirageHook* hook = nullptr;
			void Remove() override
			{
				if (hook) ::mirage::Api().hook_remove(hook);
				hook = nullptr;
			}
		};
	}

	// MIRAGE_EVENT_TICK, _LEVEL_LOAD, _DEATH, _PLAYER_STATE, _KEY (api.h says what comes with each). nullptr if it fails.
	inline Hooked* On(int32_t event, std::function<void(const MirageEvent&)> fn)
	{
		auto c = std::make_unique<detail::EventCallback>();
		c->fn = std::move(fn);
		c->sub = Api().event_subscribe(Mod(), event, [](void* u, const MirageEvent* e) {
			static_cast<detail::EventCallback*>(u)->fn(*e);
		}, c.get());
		if (!c->sub) return nullptr;
		detail::Callbacks().push_back(std::move(c));
		return detail::Callbacks().back().get();
	}

	// A game function by path ("Engine.Actor.SetLocation"); when: MIRAGE_HOOK_BEFORE / _AFTER / _REPLACE; higher
	// priority runs first. nullptr if it can't be hooked (the log says why).
	inline Hooked* Hook(const std::string& function, int32_t when, int32_t priority, std::function<void(Call&)> fn)
	{
		auto c = std::make_unique<detail::HookCallback>();
		c->fn = std::move(fn);
		c->hook = Api().hook_function(Mod(), function.c_str(), when, priority, [](void* u, MirageCall* call) {
			Call wrapped(call);
			static_cast<detail::HookCallback*>(u)->fn(wrapped);
		}, c.get());
		if (!c->hook) return nullptr;
		detail::Callbacks().push_back(std::move(c));
		return detail::Callbacks().back().get();
	}

	// ---- Registration ----------------------------------------------------------------------------------------------
	namespace detail
	{
		using Factory = std::unique_ptr<Module> (*)();
		inline std::vector<Factory>& Factories()
		{
			static std::vector<Factory> f;
			return f;
		}
		inline std::vector<std::unique_ptr<Module>>& Instances()
		{
			static std::vector<std::unique_ptr<Module>> m;
			return m;
		}
		inline int32_t Load(const MirageApi* api, MirageMod* mod)
		{
			if (!api || api->abi_version < 1) return 1;
			Api() = api;
			Mod() = mod;
			for (Factory make : Factories()) Instances().push_back(make());
			return 0;
		}
		inline void Unload()
		{
			Instances().clear();
			Callbacks().clear();		// The runtime took the hooks off already
		}
	}
}

// Registers a module class (default-constructible, derived from mirage::Module), once, at namespace scope
#define MIRAGE_CONCAT_(a, b) a##b
#define MIRAGE_CONCAT(a, b) MIRAGE_CONCAT_(a, b)
#define MIRAGE_MODULE(T)                                                                                   \
	[[maybe_unused]] static const bool MIRAGE_CONCAT(kMirageRegistered_, T) =                              \
		(::mirage::detail::Factories().push_back(                                                          \
			 []() -> std::unique_ptr<::mirage::Module> { return std::make_unique<T>(); }),                \
		 true);

// The mod's exports: once per mod, in one .cpp file
#define MIRAGE_MOD()                                                                                       \
	MIRAGE_EXTERN_C MIRAGE_EXPORT uint32_t MIRAGE_CALL mirage_mod_abi(void) { return MIRAGE_ABI_VERSION; } \
	MIRAGE_EXTERN_C MIRAGE_EXPORT int32_t MIRAGE_CALL mirage_mod_load(const MirageApi* api, MirageMod* mod) \
	{                                                                                                      \
		return ::mirage::detail::Load(api, mod);                                                           \
	}                                                                                                      \
	MIRAGE_EXTERN_C MIRAGE_EXPORT void MIRAGE_CALL mirage_mod_unload(void) { ::mirage::detail::Unload(); }
