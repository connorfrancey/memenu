#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;
struct lua_Debug;

// Lua 5.4 for Mirage mods (built from source as C++: cmake/Libraries.cmake). One State per mod: sandboxed (no io, os,
// package, debug), with a memory limit and an instruction budget per call, so a broken script is stopped and named
// instead of taking the game with it.
namespace mirage::lua
{
	struct Result
	{
		bool ok = false;
		std::string value;		// What the chunk returned (first value, as text)
		std::string error;		// Lua's message, or why it was stopped
	};

	class State
	{
	public:
		// `memoryLimit`: bytes the state may allocate. `instructionBudget`: VM instructions per Run (0: unlimited).
		explicit State(std::size_t memoryLimit = 64u << 20, std::uint64_t instructionBudget = 50'000'000);
		~State();
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		bool Valid() const { return L_ != nullptr; }
		lua_State* Get() const { return L_; }

		// Compiles and runs `code` (named `chunk` in messages). Errors, the memory limit and the budget come back in
		// Result, never as exceptions.
		Result Run(std::string_view code, std::string_view chunk = "chunk");

		std::size_t MemoryUsed() const { return used_; }
		std::size_t MemoryPeak() const { return peak_; }

	private:
		static void* Allocate(void* self, void* ptr, std::size_t oldSize, std::size_t newSize);
		static void CountHook(lua_State* L, lua_Debug* ar);

		lua_State* L_ = nullptr;
		std::size_t used_ = 0, peak_ = 0, limit_;
		std::uint64_t budget_, executed_ = 0;
	};

	// Runs the checks of Mirage step 1 in a fresh state: results, a caught error, the sandbox, C++ unwinding through a
	// Lua error, the memory limit and the instruction budget. Logged; `problems` empty when all passed.
	struct ProbeReport
	{
		bool ok = false;
		std::vector<std::string> problems;
		std::vector<std::string> notes;
		double seconds = 0.0;
	};
	ProbeReport Probe();

	const char* Version();		// "Lua 5.4.8"
}
