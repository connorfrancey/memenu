#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The process's memory and threads, sorted by who owns them, for memory snapshots (tas/snapshot.h).
//
// A snapshot may only hold and put back the game's own memory: its modules' data, the heaps and virtual memory the game
// allocates. Never the menu's, the graphics driver's, Windows' or another thread's stack: those keep living while the
// game goes back in time.
//
// Owners:
//   GameImage   writable sections of the game's modules (MirrorsEdge.exe, its CRT msvcr80/msvcp80, PhysX, ogg/vorbis...)
//   GameHeap    heaps of the game's modules: the shared CRT heap (msvcr80!_get_heap_handle) and heaps they created
//   GameVirtual VirtualAlloc'd memory the game asked for (UE3's allocator): known from the allocation tracker
//   Stack       a thread's stack (and its TEB)
//   Other       everything else: system and menu modules, other heaps, the driver, mapped files, unknown virtual memory
namespace tas::memmap {

enum class Owner : std::uint8_t { GameImage, GameHeap, GameVirtual, Stack, Other };
const char* OwnerName(Owner o);

struct Module {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::string name;       // file name, lower case
    std::string path;
    bool game = false;
};

struct Region {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::uint32_t protect = 0;
    std::uint32_t type = 0;         // MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE
    Owner owner = Owner::Other;
    std::string what;               // module, heap or thread it belongs to
};

struct Thread {
    std::uint32_t id = 0;
    std::uintptr_t start = 0;
    std::string startModule;        // module of the start address ("?" if none)
    std::uintptr_t stackLow = 0, stackHigh = 0;   // reserved stack range (DeallocationStack .. StackBase)
    std::uintptr_t teb = 0;
    std::uint32_t state = 0;        // KTHREAD_STATE (5 = waiting)
    std::uint32_t waitReason = 0;   // KWAIT_REASON (6 UserRequest, 0 Executive, ...)
    std::uint64_t cycles = 0;       // QueryThreadCycleTime
};

std::vector<Module> Modules();
// The heaps of the process and whether they are the game's: the CRT heap of msvcr80 and the heaps created by game
// modules (HeapCreate is tracked from Install()).
struct Heap {
    void* handle = nullptr;
    bool game = false;
    std::string what;
};
std::vector<Heap> Heaps();
std::vector<Thread> Threads();

// Every committed, writable, readable region, with its owner. Game regions are what a snapshot holds.
std::vector<Region> Regions();

// Starts tracking the game's VirtualAlloc / VirtualFree and HeapCreate calls (by the caller's module). Memory the game
// allocated before this is unknown (Owner::Other) unless the loader (dinput8.dll) tracked it from the start.
bool Install();
bool Tracking();

// What the allocation tracker saw, for the memory survey: bytes allocated by each calling module since Install().
struct AllocStats {
    std::string module;
    std::uint64_t calls = 0, bytes = 0, live = 0;
};
std::vector<AllocStats> AllocationCallers();

bool IsGameModule(const std::string& lowerName, const std::string& path);

// Where a thread is: its instruction pointer and the values on its stack that point into modules ("exe+0x1234"), up to
// `max`. Suspends the thread for the moment it takes. For the survey, never on the game thread.
std::vector<std::string> StackSample(std::uint32_t threadId, std::size_t max = 16);

constexpr std::size_t kPage = 4096;
// A 64-bit hash of the page at `page` (4 KiB aligned). False if it can't be read (freed meanwhile by another thread).
bool HashPage(const void* page, std::uint64_t& hash);

} // namespace tas::memmap
