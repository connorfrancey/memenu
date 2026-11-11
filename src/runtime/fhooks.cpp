#include "runtime/fhooks.h"

#include "core/clock.h"
#include "game/object_cache.h"
#include "game/uobject.h"
#include "sdk/SDK.hpp"

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mirage::hooks
{
	namespace
	{
		constexpr std::uint32_t kFuncNative = 0x400;
		constexpr int kFunctionFlags = 0x90;		// UFunction (checked by the self-tests "Mirage layouts" and "Mirage calls")
		constexpr int kSlots = 128;					// Functions hooked in a session (a slot stays with its function)

		using Native = void(__thiscall*)(Classes::UObject* self, void* stack, void* result);

		struct Hook
		{
			int id;
			When when;
			int priority;
			int order;			// Added order (ties)
			Callback callback;
			void* user;
		};

		// What runs for a hooked function: read by the game thread as an immutable snapshot, replaced when its hooks change
		struct Slot
		{
			Classes::UFunction* fn = nullptr;
			Native original = nullptr;		// Native functions: their Func before the hook
			bool native = false;
			std::vector<Hook> hooks;		// Sorted: priority, then order
		};
		std::array<std::atomic<std::shared_ptr<const Slot>>, kSlots> g_slots;

		struct Hooked			// A function asked for, by path
		{
			std::vector<Hook> hooks;
			int slot = -1;		// -1: not found (yet)
		};

		std::mutex g_lock;							// g_hooked, slots, the table's writes and Func pointers (not the calls)
		std::map<std::string, Hooked> g_hooked;
		int g_slotCount = 0;
		int g_nextId = 1;
		int g_order = 0;

		// Offsets: UFunction::Func (Calibrate), FFrame::Node and Locals (the first call through the detour)
		int g_funcOffset = -1;
		std::atomic<int> g_nodeOffset{-1};
		std::string g_why;

		std::atomic<std::uint64_t> g_calls{0};
		std::atomic<std::uint64_t> g_nanos{0};

		// Script functions: UObject::ProcessInternal runs every one of them (called directly by the VM for a call from
		// script, through UFunction::Func for one from native code or ProcessEvent). It is detoured once and looks the
		// frame's function up in this table: open addressing over the UFunction pointer, no lock. An unhooked function
		// costs a hash and a load or two.
		constexpr int kTable = 256;
		constexpr std::uintptr_t kGone = 1;		// A removed entry: probing goes on past it
		std::array<std::atomic<std::uintptr_t>, kTable> g_table{};
		std::array<std::atomic<int>, kTable> g_tableSlot{};
		Native g_processInternal = nullptr;	// Its address (UFunction::Func of every script function)
		Native g_processOriginal = nullptr;	// MinHook's trampoline to it

		std::uint32_t Dword(const void* p, int at)
		{
			std::uint32_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
			return v;
		}
		Native FuncOf(Classes::UFunction* fn) { return reinterpret_cast<Native>(Dword(fn, g_funcOffset)); }
		void SetFunc(Classes::UFunction* fn, Native f)
		{
			// One aligned dword: the VM sees the old or the new pointer, never half of each
			reinterpret_cast<std::atomic<std::uint32_t>*>(reinterpret_cast<std::uint8_t*>(fn) + g_funcOffset)
				->store(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(f)));
		}

		int TableFind(std::uintptr_t fn)
		{
			for (int i = 0, h = static_cast<int>((fn >> 4) & (kTable - 1)); i < kTable; ++i, h = (h + 1) & (kTable - 1)) {
				const std::uintptr_t v = g_table[h].load(std::memory_order_acquire);
				if (v == fn) return g_tableSlot[h].load(std::memory_order_relaxed);
				if (v == 0) return -1;
			}
			return -1;
		}
		void TableSet(std::uintptr_t fn, int slot)	// With g_lock. slot -1: removed
		{
			int free = -1;
			for (int i = 0, h = static_cast<int>((fn >> 4) & (kTable - 1)); i < kTable; ++i, h = (h + 1) & (kTable - 1)) {
				const std::uintptr_t v = g_table[h].load();
				if (v == fn) {
					if (slot < 0) g_table[h].store(kGone, std::memory_order_release);
					else g_tableSlot[h].store(slot);
					return;
				}
				if (v == kGone && free < 0) free = h;
				if (v == 0) {
					if (free < 0) free = h;
					break;
				}
			}
			if (slot < 0 || free < 0) return;
			g_tableSlot[free].store(slot);
			g_table[free].store(fn, std::memory_order_release);		// Published last
		}

		bool InExe(std::uint32_t address)
		{
			static const MODULEINFO mi = [] {
				MODULEINFO m{};
				GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &m, sizeof m);
				return m;
			}();
			const auto base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
			return address >= base && address < base + mi.SizeOfImage;
		}

		// UFunction::Func: the offset where every script function holds the same pointer into the game (ProcessInternal)
		// and native functions hold different ones, also into the game. With g_lock.
		bool Calibrate()
		{
			if (g_funcOffset >= 0) return true;
			if (!g_why.empty()) return false;
			Classes::UClass* functionClass = Classes::UFunction::StaticClass();
			if (!functionClass) return false;		// Not loaded yet: asked again later
			std::vector<Classes::UFunction*> script, natives;
			const auto& objects = Classes::UObject::GetGlobalObjects();
			for (std::size_t i = 0; i < objects.Num() && (script.size() < 300 || natives.size() < 300); ++i) {
				auto* o = objects.GetByIndex(i);
				if (!o || o->Class != functionClass) continue;
				auto* fn = static_cast<Classes::UFunction*>(o);
				((Dword(fn, kFunctionFlags) & kFuncNative) ? natives : script).push_back(fn);
			}
			if (script.size() < 50 || natives.size() < 50) return false;	// Too early
			for (int at = 0xA4; at <= 0xC8; at += 4) {
				const std::uint32_t common = Dword(script.front(), at);
				if (!InExe(common)) continue;
				if (!std::all_of(script.begin(), script.end(), [&](auto* f) { return Dword(f, at) == common; })) continue;
				std::vector<std::uint32_t> seen;
				std::size_t inExe = 0;
				for (auto* f : natives) {
					const std::uint32_t v = Dword(f, at);
					inExe += InExe(v);
					if (v != common && std::find(seen.begin(), seen.end(), v) == seen.end()) seen.push_back(v);
				}
				if (inExe == natives.size() && seen.size() > natives.size() / 2) {
					g_funcOffset = at;
					g_processInternal = reinterpret_cast<Native>(static_cast<std::uintptr_t>(common));
					spdlog::info("Function hooks: UFunction::Func at 0x{:X}, ProcessInternal at {:#x} ({} script and {} native "
								 "functions agree)", at, common, script.size(), natives.size());
					return true;
				}
			}
			g_why = "UFunction::Func wasn't found (no offset where the script functions share ProcessInternal)";
			spdlog::error("Function hooks: {}: hooks are off", g_why);
			return false;
		}

		// FFrame: Node (the function) then Object, Code, Locals. Found at the first call: the dword that is a UFunction
		// with the called object after it.
		int NodeOffset(Classes::UObject* self, void* stack)
		{
			const int known = g_nodeOffset.load(std::memory_order_relaxed);
			if (known >= 0) return known;
			static Classes::UClass* const functionClass = Classes::UFunction::StaticClass();
			for (int at = 0; at <= 0x30; at += 4) {
				if (Dword(stack, at + 4) != reinterpret_cast<std::uintptr_t>(self)) continue;
				auto* o = reinterpret_cast<Classes::UObject*>(static_cast<std::uintptr_t>(Dword(stack, at)));
				if (!o || IsBadReadPtr(o, 0x40) || o->Class != functionClass) continue;
				g_nodeOffset = at;
				spdlog::info("Function hooks: FFrame::Node at 0x{:X}, Locals at 0x{:X}", at, at + 12);
				return at;
			}
			return -1;
		}

		void Run(int index, Classes::UObject* self, void* stack, void* result, Native original)
		{
			const std::shared_ptr<const Slot> slot = g_slots[index].load(std::memory_order_acquire);
			const clk::Ticks start = clk::Now();
			Call call;
			call.object = self;
			call.function = slot->fn;
			call.native = slot->native;
			call.result = result;
			const int node = g_nodeOffset.load(std::memory_order_relaxed);
			if (!slot->native && node >= 0)		// Its own frame: the parameters in its Locals
				call.params = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(Dword(stack, node + 12)));

			for (const Hook& k : slot->hooks) {
				if (k.when == When::After) continue;
				if (k.when == When::Replace) {
					if (call.skip || slot->native) continue;	// The first replace decides
					call.skip = true;
				}
				k.callback(k.user, call);
			}
			if (slot->native) call.skip = false;	// Its code must read its parameters from the caller's bytecode
			const clk::Ticks before = clk::Now();
			if (!call.skip) original(self, stack, result);
			const clk::Ticks after = clk::Now();
			for (const Hook& k : slot->hooks)
				if (k.when == When::After) k.callback(k.user, call);
			// The time around the original: what hooking costs (the original's own time isn't counted)
			const double micros = clk::Micros(before - start) + clk::Micros(clk::Now() - after);
			g_calls.fetch_add(1, std::memory_order_relaxed);
			g_nanos.fetch_add(static_cast<std::uint64_t>(micros * 1000.0), std::memory_order_relaxed);
		}

		void __fastcall ProcessInternalDetour(Classes::UObject* self, void*, void* stack, void* result)
		{
			const int at = NodeOffset(self, stack);
			if (at >= 0) {
				const int slot = TableFind(Dword(stack, at));
				if (slot >= 0) return Run(slot, self, stack, result, g_processOriginal);
			}
			g_processOriginal(self, stack, result);
		}

		// Native functions: one thunk per slot in their Func (the VM calls Func as a __thiscall (this, FFrame&, result))
		template <int I>
		void __fastcall SlotThunk(Classes::UObject* self, void*, void* stack, void* result)
		{
			Run(I, self, stack, result, g_slots[I].load(std::memory_order_acquire)->original);
		}
		template <int... I>
		constexpr std::array<Native, sizeof...(I)> MakeThunks(std::integer_sequence<int, I...>)
		{
			return {reinterpret_cast<Native>(&SlotThunk<I>)...};
		}
		const std::array<Native, kSlots> kThunks = MakeThunks(std::make_integer_sequence<int, kSlots>{});

		// Detours ProcessInternal once (MinHook is set up by the menu's hooks). With g_lock.
		bool HookProcessInternal()
		{
			if (g_processOriginal) return true;
			if (!g_processInternal) return false;
			if (MH_CreateHook(reinterpret_cast<void*>(g_processInternal), reinterpret_cast<void*>(&ProcessInternalDetour),
							  reinterpret_cast<void**>(&g_processOriginal)) != MH_OK ||
				MH_EnableHook(reinterpret_cast<void*>(g_processInternal)) != MH_OK) {
				g_processOriginal = nullptr;
				g_why = "UObject::ProcessInternal can't be detoured";
				spdlog::error("Function hooks: {}", g_why);
				return false;
			}
			spdlog::info("Function hooks: ProcessInternal detoured");
			return true;
		}

		// The slot a function's hooks are read from, published. With g_lock.
		void Publish(int index, const Hooked& h)
		{
			auto s = std::make_shared<Slot>(*g_slots[index].load());
			s->hooks = h.hooks;
			g_slots[index].store(std::move(s), std::memory_order_release);
		}

		// The function `path` names, hooked if it is loaded. With g_lock.
		void Install(const std::string& path, Hooked& h)
		{
			if (h.slot >= 0 || !Calibrate()) return;
			auto* fn = game::FindObject<Classes::UFunction>("Function " + path);
			if (!fn) return;
			int index = -1;
			for (int i = 0; i < g_slotCount; ++i)		// Hooked before in this session: its slot again
				if (g_slots[i].load()->fn == fn) index = i;
			if (index < 0) {
				if (g_slotCount == kSlots) {
					spdlog::error("Function hooks: more than {} functions hooked; {} isn't", kSlots, path);
					return;
				}
				auto s = std::make_shared<Slot>();
				s->fn = fn;
				s->native = (Dword(fn, kFunctionFlags) & kFuncNative) != 0;
				s->original = FuncOf(fn);
				if (!s->native && !HookProcessInternal()) return;
				index = g_slotCount++;
				g_slots[index].store(std::move(s));
			}
			h.slot = index;
			Publish(index, h);
			const bool native = g_slots[index].load()->native;
			if (native) SetFunc(fn, kThunks[index]);
			else TableSet(reinterpret_cast<std::uintptr_t>(fn), index);
			spdlog::info("Function hooks: {} hooked{}", path, native ? " (native: before and after, no parameters)" : "");
		}

		void Uninstall(const std::string& path, Hooked& h)
		{
			if (h.slot < 0) return;
			const auto slot = g_slots[h.slot].load();
			if (slot->native) SetFunc(slot->fn, slot->original);
			else TableSet(reinterpret_cast<std::uintptr_t>(slot->fn), -1);
			h.hooks.clear();
			Publish(h.slot, h);			// A call already running finishes without hooks
			h.slot = -1;
			spdlog::info("Function hooks: {} unhooked", path);
		}
	}

	reflect::Value Call::Param(std::string_view path) const
	{
		reflect::Value v;
		if (!params) {
			v.error = native ? "a native function's parameters aren't reachable" : "the parameters weren't found";
			return v;
		}
		if (path == "ReturnValue" || path.rfind("ReturnValue.", 0) == 0) {
			// The return value goes to `result`, not into the locals
			const reflect::Member* m = reflect::Find(reinterpret_cast<Classes::UStruct*>(function), "ReturnValue");
			if (!m || !result) {
				v.error = "the function returns nothing";
				return v;
			}
			if (path == "ReturnValue") {
				v.member = m;
				v.address = static_cast<std::uint8_t*>(result);
				return v;
			}
			return reflect::Resolve(m->structType, result, path.substr(12));
		}
		return reflect::Resolve(reinterpret_cast<Classes::UStruct*>(function), params, path);
	}

	int Add(const std::string& function, When when, int priority, Callback callback, void* user, std::string* error)
	{
		if (!callback) return 0;
		std::lock_guard lock(g_lock);
		Hooked& h = g_hooked[function];
		const bool fresh = h.hooks.empty();
		Install(function, h);
		const auto fail = [&](std::string why) {
			if (error) *error = std::move(why);
			if (fresh) {
				Uninstall(function, h);
				g_hooked.erase(function);
			}
			return 0;
		};
		if (!g_why.empty()) return fail("function hooks don't work in this game: " + g_why);
		if (h.slot >= 0 && g_slots[h.slot].load()->native && when == When::Replace)
			return fail(function + " is a native function: it can be hooked before and after, not replaced");
		const int id = g_nextId++;
		h.hooks.push_back({id, when, priority, g_order++, callback, user});
		std::stable_sort(h.hooks.begin(), h.hooks.end(), [](const Hook& a, const Hook& b) {
			return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
		});
		if (h.slot >= 0) Publish(h.slot, h);
		else spdlog::info("Function hooks: {} isn't loaded yet: hooked when it is", function);
		return id;
	}

	void Remove(int id)
	{
		std::lock_guard lock(g_lock);
		for (auto it = g_hooked.begin(); it != g_hooked.end(); ++it) {
			Hooked& h = it->second;
			const auto before = h.hooks.size();
			std::erase_if(h.hooks, [&](const Hook& k) { return k.id == id; });
			if (h.hooks.size() == before) continue;
			if (h.hooks.empty()) {
				Uninstall(it->first, h);
				g_hooked.erase(it);
			} else if (h.slot >= 0) {
				Publish(h.slot, h);
			}
			return;
		}
	}

	void RemoveAll()
	{
		std::lock_guard lock(g_lock);
		for (auto& [path, h] : g_hooked) Uninstall(path, h);
		g_hooked.clear();
		if (g_processOriginal) MH_RemoveHook(reinterpret_cast<void*>(g_processInternal));
		g_processOriginal = nullptr;
	}

	void Retry()
	{
		std::lock_guard lock(g_lock);
		for (auto& [path, h] : g_hooked) {
			if (h.slot >= 0) {
				// The function went with its level's package: looked for again
				if (game::FindObject<Classes::UFunction>("Function " + path) == g_slots[h.slot].load()->fn) continue;
				Hooked keep = h;
				Uninstall(path, h);
				h.hooks = std::move(keep.hooks);
			}
			Install(path, h);
		}
	}

	bool Ready(std::string* why)
	{
		std::lock_guard lock(g_lock);
		const bool ok = Calibrate();
		if (!ok && why) *why = g_why.empty() ? "not calibrated yet (too few functions loaded)" : g_why;
		return ok;
	}

	Stats GetStats()
	{
		std::lock_guard lock(g_lock);
		Stats s;
		s.calls = g_calls.load();
		s.microseconds = static_cast<double>(g_nanos.load()) / 1000.0;
		for (const auto& [path, h] : g_hooked) {
			s.functions += h.slot >= 0;
			s.hooks += static_cast<int>(h.hooks.size());
		}
		return s;
	}
}
