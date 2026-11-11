#include "runtime/mod_game.h"

#include "game/game.h"
#include "game/object_cache.h"
#include "game/uobject.h"
#include "runtime/call.h"
#include "runtime/fhooks.h"
#include "runtime/mod_internal.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// A call of a game function, as a mod sees it: one a hook runs in, or one the mod makes
struct MirageCall
{
	mirage::hooks::Call* hook = nullptr;				// A hooked call
	std::unique_ptr<mirage::call::Params> params;		// A call the mod makes
	Classes::UObject* object = nullptr;
	std::string function;
};

namespace mirage::mods
{
	namespace
	{
		Classes::UObject* Obj(MirageObject* o) { return reinterpret_cast<Classes::UObject*>(o); }
		MirageObject* Handle(Classes::UObject* o) { return reinterpret_cast<MirageObject*>(o); }

		// The object is still one the engine has: a mod may hold a pointer past the object's life. The read of its
		// index is guarded (the memory may be gone), then the object list must hold it at that index.
		bool ReadIndex(const Classes::UObject* o, std::size_t* index)
		{
			__try {
				*index = static_cast<std::size_t>(o->ObjectInternalInteger);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		bool Alive(Classes::UObject* o)
		{
			std::size_t index = 0;
			return o && Classes::UObject::GObjects && ReadIndex(o, &index) && game::AliveAt(o, index);
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

		int32_t Read(const reflect::Value& v, MirageValue* out)
		{
			*out = MirageValue{};
			if (!v) return 0;
			using K = layout::Kind;
			switch (v.member->kind) {
				case K::Bool: out->type = MIRAGE_VALUE_BOOL; out->i = reflect::GetInt(v); break;
				case K::Int: case K::Byte: out->type = MIRAGE_VALUE_INT; out->i = reflect::GetInt(v); break;
				case K::Float: out->type = MIRAGE_VALUE_FLOAT; out->f = reflect::GetFloat(v); break;
				case K::Object: case K::Class: case K::Component: case K::Interface:
					out->type = MIRAGE_VALUE_OBJECT;
					out->o = Handle(reflect::GetObj(v));
					break;
				case K::Name: case K::Str: {
					out->type = MIRAGE_VALUE_TEXT;
					const std::string t = reflect::GetText(v);
					CopyOut(t, out->text, sizeof out->text);
					break;
				}
				case K::Struct: out->type = MIRAGE_VALUE_STRUCT; break;
				default: return 0;
			}
			return 1;
		}

		int32_t Write(const reflect::Value& v, const MirageValue* in)
		{
			if (!v || !in) return 0;
			switch (in->type) {
				case MIRAGE_VALUE_BOOL: case MIRAGE_VALUE_INT: return reflect::SetInt(v, in->i) ? 1 : 0;
				case MIRAGE_VALUE_FLOAT: return reflect::SetFloat(v, in->f) ? 1 : 0;
				case MIRAGE_VALUE_OBJECT:
					if (in->o && !Alive(Obj(in->o))) return 0;
					return reflect::SetObj(v, Obj(in->o)) ? 1 : 0;
				case MIRAGE_VALUE_TEXT: {
					char text[sizeof in->text + 1]{};
					std::memcpy(text, in->text, sizeof in->text);
					return reflect::SetText(v, text) ? 1 : 0;
				}
				default: return 0;
			}
		}

		std::string FunctionPath(Classes::UObject* fn)
		{
			std::string path = fn ? fn->GetName() : std::string();
			for (auto* o = fn ? fn->Outer : nullptr; o; o = o->Outer) path = o->GetName() + "." + path;
			return path;
		}

		// ---- Hooks: the runtime's callback around a mod's ----------------------------------------------------------
		struct HookArgs
		{
			HookRecord* record;
			MirageCall* call;
		};
		void RunModHook(void* a)
		{
			auto* args = static_cast<HookArgs*>(a);
			args->record->fn(args->record->user, args->call);
		}
		void OnHook(void* user, hooks::Call& c)
		{
			auto* r = static_cast<HookRecord*>(user);
			if (!r->id || !detail::Running(*r->mod)) return;
			MirageCall call;
			call.hook = &c;
			call.object = c.object;
			call.function = r->function;
			HookArgs args{r, &call};
			detail::Enter(*r->mod, "hook", r->function, &RunModHook, &args);
		}

		// ---- Events ------------------------------------------------------------------------------------------------
		std::mutex g_subLock;
		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<Subscription>>>> g_subs{
			std::make_shared<const std::vector<std::shared_ptr<Subscription>>>()};

		void Republish()	// With g_subLock: the active subscriptions, as one snapshot the dispatchers read
		{
			auto next = std::make_shared<std::vector<std::shared_ptr<Subscription>>>();
			for (const auto& s : *g_subs.load())
				if (s->active.load()) next->push_back(s);
			g_subs.store(std::move(next));
		}

		struct EventArgs
		{
			Subscription* sub;
			const MirageEvent* event;
		};
		void RunEvent(void* a)
		{
			auto* args = static_cast<EventArgs*>(a);
			args->sub->fn(args->sub->user, args->event);
		}
		void Dispatch(const MirageEvent& e)
		{
			const auto subs = g_subs.load(std::memory_order_acquire);
			if (subs->empty()) return;
			static const char* const kNames[] = {"tick", "level load", "death", "player state", "key"};
			for (const auto& s : *subs) {
				if (s->type != e.type || !s->active.load(std::memory_order_relaxed)) continue;
				EventArgs args{s.get(), &e};
				detail::Enter(*s->mod, "event", std::string(kNames[e.type]) + " event", &RunEvent, &args);
			}
		}
		MirageEvent Event(int32_t type)
		{
			MirageEvent e{};
			e.size = sizeof e;
			e.type = type;
			e.text = "";
			return e;
		}
	}

	void detail::RemoveGameHooks(MirageMod& mod)
	{
		for (auto& r : mod.hooks)
			if (r->id) {
				hooks::Remove(r->id);
				r->id = 0;
			}
		{
			std::lock_guard lock(g_subLock);
			for (auto& s : mod.subscriptions) s->active = false;
			Republish();
		}
		mod.subscriptions.clear();
	}

	void DispatchTick(float seconds)
	{
		MirageEvent e = Event(MIRAGE_EVENT_TICK);
		e.number = seconds;
		Dispatch(e);
	}
	void DispatchLevelLoad(bool finished, const std::string& map)
	{
		MirageEvent e = Event(MIRAGE_EVENT_LEVEL_LOAD);
		e.value = finished ? 1 : 0;
		e.text = map.c_str();
		Dispatch(e);
	}
	void DispatchDeath(bool after)
	{
		MirageEvent e = Event(MIRAGE_EVENT_DEATH);
		e.value = after ? 1 : 0;
		Dispatch(e);
	}
	void DispatchPlayerState()
	{
		static int previous = -1;
		auto* pawn = game::Pawn();
		if (!pawn) return;
		MirageEvent e = Event(MIRAGE_EVENT_PLAYER_STATE);
		e.value = static_cast<int32_t>(static_cast<std::uint8_t>(pawn->MovementState.GetValue()));
		e.value2 = previous;
		previous = e.value;
		Dispatch(e);
	}
	void DispatchKey(int vk)
	{
		MirageEvent e = Event(MIRAGE_EVENT_KEY);
		e.value = vk;
		Dispatch(e);
	}

	namespace game_api
	{
		MirageSubscription* MIRAGE_CALL EventSubscribe(MirageMod* mod, int32_t type, MirageEventFn fn, void* user)
		{
			if (!mod || !fn || type < MIRAGE_EVENT_TICK || type > MIRAGE_EVENT_KEY) return nullptr;
			auto s = std::make_shared<Subscription>();
			s->mod = mod;
			s->type = type;
			s->fn = fn;
			s->user = user;
			std::lock_guard lock(g_subLock);
			mod->subscriptions.push_back(s);
			auto next = std::make_shared<std::vector<std::shared_ptr<Subscription>>>(*g_subs.load());
			next->push_back(s);
			g_subs.store(std::move(next));
			return reinterpret_cast<MirageSubscription*>(s.get());
		}

		void MIRAGE_CALL EventUnsubscribe(MirageSubscription* subscription)
		{
			if (!subscription) return;
			auto* s = reinterpret_cast<Subscription*>(subscription);
			std::lock_guard lock(g_subLock);
			s->active = false;
			Republish();
		}

		MirageHook* MIRAGE_CALL HookFunction(MirageMod* mod, const char* function, int32_t when, int32_t priority,
											 MirageHookFn fn, void* user)
		{
			if (!mod || !function || !fn || when < MIRAGE_HOOK_BEFORE || when > MIRAGE_HOOK_REPLACE) return nullptr;
			auto r = std::make_unique<HookRecord>();
			r->mod = mod;
			r->fn = fn;
			r->user = user;
			r->function = function;
			std::string error;
			const auto w = when == MIRAGE_HOOK_BEFORE ? hooks::When::Before : when == MIRAGE_HOOK_AFTER ? hooks::When::After
																									  : hooks::When::Replace;
			r->id = hooks::Add(function, w, priority, &OnHook, r.get(), &error);
			if (!r->id) {
				spdlog::error("[mod {}] hook_function {}: {}", mod->id, function, error);
				return nullptr;
			}
			HookRecord* raw = r.get();
			mod->hooks.push_back(std::move(r));
			return reinterpret_cast<MirageHook*>(raw);
		}

		void MIRAGE_CALL HookRemove(MirageHook* hook)
		{
			auto* r = reinterpret_cast<HookRecord*>(hook);
			if (!r || !r->id) return;
			hooks::Remove(r->id);
			r->id = 0;
		}

		MirageObject* MIRAGE_CALL CallObject(MirageCall* call) { return call ? Handle(call->object) : nullptr; }
		const char* MIRAGE_CALL CallFunction(MirageCall* call) { return call ? call->function.c_str() : ""; }
		void MIRAGE_CALL CallSkip(MirageCall* call)
		{
			if (call && call->hook && !call->hook->native) call->hook->skip = true;
		}

		int32_t MIRAGE_CALL CallGet(MirageCall* call, const char* path, MirageValue* out)
		{
			if (!call || !path || !out) return 0;
			if (call->hook) return Read(call->hook->Param(path), out);
			if (!call->params) return 0;
			return Read(reflect::Resolve(reinterpret_cast<Classes::UStruct*>(call->params->Function()), call->params->Data(), path), out);
		}

		int32_t MIRAGE_CALL CallSet(MirageCall* call, const char* path, const MirageValue* in)
		{
			if (!call || !path) return 0;
			if (call->hook) return Write(call->hook->Param(path), in);
			if (!call->params) return 0;
			return Write(reflect::Resolve(reinterpret_cast<Classes::UStruct*>(call->params->Function()), call->params->Data(), path), in);
		}

		MirageCall* MIRAGE_CALL CallBegin(MirageObject* object, const char* function)
		{
			if (!object || !function || !Alive(Obj(object))) return nullptr;
			std::string_view name = function;
			if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) name.remove_prefix(dot + 1);
			auto params = std::make_unique<call::Params>(Obj(object), name);
			if (!params->Valid()) return nullptr;
			auto* c = new MirageCall;
			c->object = Obj(object);
			c->function = FunctionPath(reinterpret_cast<Classes::UObject*>(params->Function()));
			c->params = std::move(params);
			return c;
		}

		int32_t MIRAGE_CALL CallInvoke(MirageCall* call)
		{
			if (!call || !call->params || !Alive(call->object)) return 0;
			std::string error;
			if (call::Call(call->object, *call->params, &error)) return 1;
			spdlog::warn("Mods: call_invoke {}: {}", call->function, error);
			return 0;
		}

		void MIRAGE_CALL CallEnd(MirageCall* call)
		{
			if (call && !call->hook) delete call;	// A hook's call belongs to the hook
		}

		MirageObject* MIRAGE_CALL PlayerPawn() { return Handle(game::Pawn()); }
		MirageObject* MIRAGE_CALL PlayerController() { return Handle(game::Controller()); }
		MirageObject* MIRAGE_CALL WorldInfo() { return Handle(game::World()); }
		MirageObject* MIRAGE_CALL ObjectFind(const char* fullName)
		{
			return fullName ? Handle(game::FindObject<Classes::UObject>(fullName)) : nullptr;
		}

		int32_t MIRAGE_CALL ObjectGet(MirageObject* object, const char* path, MirageValue* out)
		{
			if (!out) return 0;
			*out = MirageValue{};
			if (!object || !path || !Alive(Obj(object))) return 0;
			return Read(reflect::Resolve(Obj(object)->Class, Obj(object), path), out);
		}

		int32_t MIRAGE_CALL ObjectSet(MirageObject* object, const char* path, const MirageValue* in)
		{
			if (!object || !path || !Alive(Obj(object))) return 0;
			return Write(reflect::Resolve(Obj(object)->Class, Obj(object), path), in);
		}

		uint32_t MIRAGE_CALL ObjectName(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? reflect::NameOf(Obj(object)) : std::string(), buffer, capacity);
		}

		uint32_t MIRAGE_CALL ObjectClassName(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? reflect::ClassNameOf(Obj(object)) : std::string(), buffer, capacity);
		}

		int32_t MIRAGE_CALL ObjectIsA(MirageObject* object, const char* className)
		{
			if (!object || !className || !Alive(Obj(object))) return 0;
			int depth = 0;
			for (Classes::UStruct* c = Obj(object)->Class; c && depth < 64; c = game::SuperOf(c), ++depth)
				if (c->GetName() == className) return 1;
			return 0;
		}
	}
}
