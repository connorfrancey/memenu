#pragma once
// The game's memory, tracked by the loader from the moment the game starts, for the menu's TAS memory snapshots.
//
// Every VirtualAlloc reservation MirrorsEdge.exe itself makes (UE3's allocator) is made with MEM_WRITE_WATCH, so
// Windows keeps a list of the pages written since the last look (GetWriteWatch), and recorded here. The menu loads
// minutes later; without this it couldn't tell the game's memory from the driver's, nor which pages a frame changed.
//
// The menu can also hold back the game's VirtualFree(MEM_RELEASE) and CloseHandle calls while a TAS session keeps
// snapshots: memory or a handle the game let go of may come back to life when an older snapshot is put back.
//
// Shared by the loader (dinput8.dll) and the menu (medge_menu.dll): plain C layout, versioned.
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace memenu_tracker {

constexpr std::uint32_t kVersion = 1;

struct Tracker {
    std::uint32_t version;
    std::uint32_t size;                         // sizeof(Tracker)
    std::uintptr_t exeBase, exeSize;            // the range a caller must be in to count as the game

    // The real functions (calling these is never tracked nor held back)
    LPVOID(WINAPI* VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
    BOOL(WINAPI* VirtualFree)(LPVOID, SIZE_T, DWORD);
    BOOL(WINAPI* CloseHandle)(HANDLE);

    // Every reservation of the game's that is still reserved (held back ones too). `fn` runs with the table locked:
    // keep it short and don't allocate virtual memory in it.
    void(WINAPI* ForEach)(void(WINAPI* fn)(std::uintptr_t base, std::size_t size, void* ctx), void* ctx);
    // Releases a reservation the menu held back (VirtualFree(MEM_RELEASE)) and forgets it.
    BOOL(WINAPI* Release)(std::uintptr_t base);
    std::uint32_t(WINAPI* Count)();             // reservations in the table
    std::uint64_t(WINAPI* Sequence)();          // reservations made since the game started (a change counter)

    // Set by the menu (null: not held back). Called on the game's thread with no lock held. Return TRUE to keep the
    // call from Windows (the game is told it succeeded).
    BOOL(WINAPI* volatile holdFree)(std::uintptr_t base);
    BOOL(WINAPI* volatile holdClose)(HANDLE handle);
    // VirtualFree(MEM_DECOMMIT) of the game's (size 0: to the end of the reservation's region).
    BOOL(WINAPI* volatile holdDecommit)(std::uintptr_t address, std::size_t size);
    // A new reservation of the game's (VirtualAlloc without an address): return the address to hand the game (one made
    // before, to replay the allocations of frames that ran before, or one the callback made itself through the real
    // VirtualAlloc, with MEM_WRITE_WATCH), or null to let the loader make it. Either way it goes into the table.
    LPVOID(WINAPI* volatile replayAlloc)(SIZE_T size, DWORD type, DWORD protect);

    // Who allocated each TLS index (TlsAlloc's return address, 0: free or allocated before the loader). PhysX keeps
    // per-thread data behind TLS: a snapshot has to put those slots back too.
    const volatile std::uintptr_t* tlsOwners;
    std::uint32_t tlsIndices;                   // entries in tlsOwners (TLS_MINIMUM_AVAILABLE + expansion slots)

    BOOL(WINAPI* CloseGameHandle)(HANDLE handle);   // a close the menu held back, done now

    bool writeWatch;                            // MEM_WRITE_WATCH is supported (false: tracked without it)
    std::uint32_t dropped;                      // reservations not recorded: the table was full
};

// dinput8.dll export; null from a loader without it.
using GetTracker_t = Tracker*(WINAPI*)();
constexpr const char* kExport = "MemenuGetTracker";

} // namespace memenu_tracker
