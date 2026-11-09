#include "runtime/lua_vm.h"

#include "core/clock.h"

// Lua is compiled as C++ here (cmake/Libraries.cmake), so its headers are included as they are: lua.hpp would wrap
// them in extern "C" and the names would not match the library's.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>

namespace mirage::lua
{
	namespace
	{
		constexpr int kHookEvery = 1000;		// Instructions between budget checks

		// The libraries a mod gets: no io, os, package (loadlib, require from anywhere) or debug
		void OpenSafeLibraries(lua_State* L)
		{
			const luaL_Reg libs[] = {
				{LUA_GNAME, luaopen_base},       {LUA_TABLIBNAME, luaopen_table},   {LUA_STRLIBNAME, luaopen_string},
				{LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},   {LUA_COLIBNAME, luaopen_coroutine},
			};
			for (const luaL_Reg& lib : libs) {
				luaL_requiref(L, lib.name, lib.func, 1);
				lua_pop(L, 1);
			}
			// Base functions that reach the file system
			for (const char* name : {"dofile", "loadfile"}) {
				lua_pushnil(L);
				lua_setglobal(L, name);
			}
			// load() only takes source text: precompiled bytecode can break out of any sandbox
			luaL_dostring(L, "local load = load "
							 "_G.load = function(chunk, name, _, env) return load(chunk, name, 't', env) end");
		}

		std::string ToText(lua_State* L, int index)
		{
			std::size_t len = 0;
			const char* s = luaL_tolstring(L, index, &len);		// Pushes the text
			std::string out(s ? s : "", len);
			lua_pop(L, 1);
			return out;
		}
	}

	State::State(std::size_t memoryLimit, std::uint64_t instructionBudget) : limit_(memoryLimit), budget_(instructionBudget)
	{
		L_ = lua_newstate(&Allocate, this);
		if (!L_) return;
		// Keep `this` reachable from the hook
		*static_cast<State**>(lua_getextraspace(L_)) = this;
		OpenSafeLibraries(L_);
		if (budget_) lua_sethook(L_, &CountHook, LUA_MASKCOUNT, kHookEvery);
	}

	State::~State()
	{
		if (L_) lua_close(L_);
	}

	void* State::Allocate(void* self, void* ptr, std::size_t oldSize, std::size_t newSize)
	{
		auto* s = static_cast<State*>(self);
		const std::size_t old = ptr ? oldSize : 0;		// Without ptr, oldSize is the kind of object being made
		if (newSize == 0) {
			std::free(ptr);
			s->used_ -= old;
			return nullptr;
		}
		if (newSize > old && s->used_ - old + newSize > s->limit_) return nullptr;	// Lua raises "not enough memory"
		void* p = std::realloc(ptr, newSize);
		if (!p) return nullptr;
		s->used_ = s->used_ - old + newSize;
		if (s->used_ > s->peak_) s->peak_ = s->used_;
		return p;
	}

	void State::CountHook(lua_State* L, lua_Debug*)
	{
		State* s = *static_cast<State**>(lua_getextraspace(L));
		s->executed_ += kHookEvery;
		if (s->budget_ && s->executed_ > s->budget_)
			luaL_error(L, "stopped: more than %d million instructions in one call", static_cast<int>(s->budget_ / 1'000'000));
	}

	Result State::Run(std::string_view code, std::string_view chunk)
	{
		Result r;
		if (!L_) {
			r.error = "no Lua state (out of memory)";
			return r;
		}
		executed_ = 0;
		const int top = lua_gettop(L_);
		const std::string name = "=" + std::string(chunk);
		int status = luaL_loadbufferx(L_, code.data(), code.size(), name.c_str(), "t");		// Text only, no bytecode
		if (status == LUA_OK) status = lua_pcall(L_, 0, 1, 0);
		if (status == LUA_OK) {
			r.ok = true;
			if (!lua_isnil(L_, -1)) r.value = ToText(L_, -1);
		} else {
			r.error = lua_type(L_, -1) == LUA_TSTRING ? lua_tostring(L_, -1)
					: status == LUA_ERRMEM ? std::string("not enough memory")
					: "error (" + std::to_string(status) + ")";
		}
		lua_settop(L_, top);
		return r;
	}

	const char* Version() { return LUA_RELEASE; }

	namespace
	{
		int g_destroyed = 0;
		struct Guard { ~Guard() { ++g_destroyed; } };

		// A C function holding a C++ object with a destructor when it raises a Lua error: compiled as C++, the error
		// is an exception and the destructor runs; with longjmp it would be skipped.
		int RaiseWithGuard(lua_State* L)
		{
			Guard guard;
			return luaL_error(L, "raised from C++");
		}
	}

	ProbeReport Probe()
	{
		ProbeReport rep;
		const clk::Ticks start = clk::Now();
		const auto expect = [&](const char* what, const Result& r, bool wantOk, std::string_view want) {
			const std::string& got = wantOk ? r.value : r.error;
			if (r.ok != wantOk || got.find(want) == std::string::npos)
				rep.problems.push_back(std::string(what) + ": got " + (r.ok ? "'" + r.value + "'" : "error '" + r.error + "'") +
									   ", wanted " + (wantOk ? "'" : "an error with '") + std::string(want) + "'");
			else
				rep.notes.push_back(std::string(what) + ": " + (r.ok ? r.value : r.error));
		};

		{
			State s;
			if (!s.Valid()) {
				rep.problems.push_back("lua_newstate failed");
				return rep;
			}
			expect("result", s.Run("return 6 * 7", "answer"), true, "42");
			expect("tables and strings",
				   s.Run("local t = {} for i = 1, 1000 do t[i] = i * i end "
						 "return #t .. ':' .. string.format('%.1f', math.sqrt(t[1000])) .. ':' .. math.type(t[3])", "tables"),
				   true, "1000:1000.0:integer");
			expect("error caught", s.Run("error('boom')", "boom"), false, "boom:1: boom");
			expect("syntax error", s.Run("return (", "syntax"), false, "syntax:");
			expect("sandbox", s.Run("return tostring(io == nil and os == nil and package == nil and debug == nil "
									"and dofile == nil and loadfile == nil)", "sandbox"), true, "true");
			expect("no bytecode", s.Run("return tostring(load(string.dump(function() end)))", "bytecode"), true, "nil");

			lua_register(s.Get(), "raise_with_guard", &RaiseWithGuard);
			g_destroyed = 0;
			expect("C++ unwinding", s.Run("raise_with_guard()", "unwind"), false, "raised from C++");
			if (g_destroyed != 1)
				rep.problems.push_back("a Lua error skipped a C++ destructor (" + std::to_string(g_destroyed) +
									   " ran): Lua is not compiled as C++");
			expect("state still usable", s.Run("return 'still fine'", "after"), true, "still fine");
		}
		{
			State s(16u << 20);
			expect("memory limit", s.Run("local t = {} for i = 1, 100 do t[i] = string.rep('x', 1 << 20) .. i end", "memory"),
				   false, "not enough memory");
			expect("after memory limit", s.Run("collectgarbage() return 1 + 1", "after"), true, "2");
			rep.notes.push_back("peak " + std::to_string(s.MemoryPeak() >> 20) + " MB of 16 MB");
		}
		{
			State s(64u << 20, 10'000'000);
			expect("instruction budget", s.Run("while true do end", "loop"), false, "stopped: more than 10 million");
			expect("after budget", s.Run("local n = 0 for i = 1, 1000 do n = n + i end return n", "after"), true, "500500");
		}

		rep.seconds = clk::SecondsSince(start);
		rep.ok = rep.problems.empty();
		if (rep.ok) spdlog::info("Mirage: {} works in the game ({} checks, {:.3f} s)", Version(), rep.notes.size(), rep.seconds);
		else
			for (const std::string& p : rep.problems) spdlog::error("Mirage: Lua: {}", p);
		return rep;
	}
}
