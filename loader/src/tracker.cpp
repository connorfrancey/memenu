// The game's memory reservations from the start of the process (see tracker.h).
#include "tracker.h"

#include <MinHook.h>

#include <intrin.h>

namespace memenu_tracker {

namespace {

using VirtualAlloc_t = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFree_t = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);

VirtualAlloc_t oVirtualAlloc = nullptr;
using TlsAlloc_t = DWORD(WINAPI*)();
using TlsFree_t = BOOL(WINAPI*)(DWORD);
TlsAlloc_t oTlsAlloc = nullptr;
TlsFree_t oTlsFree = nullptr;
constexpr std::uint32_t kTlsIndices = 64 + 1024;
volatile std::uintptr_t g_tlsOwners[kTlsIndices];
VirtualFree_t oVirtualFree = nullptr;
CloseHandle_t oCloseHandle = nullptr;

// Open addressing table base -> size, linear probing with backward shift deletion. No heap: it is used from inside
// VirtualAlloc, from the first moments of the process.
constexpr std::uint32_t kSlots = 1u << 17;          // 128k reservations (the game has ~10k live)
struct Slot {
    std::uintptr_t base;                            // 0 = empty
    std::size_t size;
};
Slot g_slots[kSlots];
std::uint32_t g_count = 0;
std::uint64_t g_sequence = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
Tracker g_tracker{};

std::uint32_t Hash(std::uintptr_t base)
{
    // Reservations are 64 KiB aligned
    return static_cast<std::uint32_t>((base >> 16) * 2654435761u) & (kSlots - 1);
}

void Insert(std::uintptr_t base, std::size_t size)
{
    AcquireSRWLockExclusive(&g_lock);
    ++g_sequence;
    if (g_count + 1 >= kSlots * 3 / 4) {
        ++g_tracker.dropped;
    } else {
        std::uint32_t i = Hash(base);
        while (g_slots[i].base && g_slots[i].base != base) i = (i + 1) & (kSlots - 1);
        if (!g_slots[i].base) ++g_count;
        g_slots[i] = {base, size};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool Contains(std::uintptr_t base)
{
    AcquireSRWLockShared(&g_lock);
    std::uint32_t i = Hash(base);
    while (g_slots[i].base && g_slots[i].base != base) i = (i + 1) & (kSlots - 1);
    const bool found = g_slots[i].base == base;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void Remove(std::uintptr_t base)
{
    AcquireSRWLockExclusive(&g_lock);
    std::uint32_t i = Hash(base);
    while (g_slots[i].base && g_slots[i].base != base) i = (i + 1) & (kSlots - 1);
    if (g_slots[i].base == base) {
        // Backward shift: move later entries of the same probe run into the hole
        std::uint32_t hole = i;
        for (std::uint32_t j = (i + 1) & (kSlots - 1); g_slots[j].base; j = (j + 1) & (kSlots - 1)) {
            const std::uint32_t home = Hash(g_slots[j].base);
            // j may move to the hole if its home is not in (hole, j] (cyclically)
            const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
            if (!between) {
                g_slots[hole] = g_slots[j];
                hole = j;
            }
        }
        g_slots[hole] = {0, 0};
        --g_count;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool FromGame(const void* returnAddress)
{
    const auto a = reinterpret_cast<std::uintptr_t>(returnAddress);
    return a - g_tracker.exeBase < g_tracker.exeSize;
}

LPVOID WINAPI VirtualAllocHook(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    // A new reservation: MEM_RESERVE, or MEM_COMMIT without an address (Windows reserves then; UE3's allocator does
    // that). MEM_WRITE_WATCH needs MEM_RESERVE spelled out.
    const bool reserves = (type & MEM_RESERVE) || (!address && (type & MEM_COMMIT));
    const bool game = reserves && FromGame(_ReturnAddress()) && !(type & (MEM_LARGE_PAGES | MEM_PHYSICAL));
    if (!game) return oVirtualAlloc(address, size, type, protect);
    type |= MEM_RESERVE;

    if (!address)
        if (auto replay = g_tracker.replayAlloc)
            if (LPVOID again = replay(size, type, protect)) {
                // The menu's: one made earlier handed out again, or a new one it made itself (with write watching)
                Insert(reinterpret_cast<std::uintptr_t>(again), (size + 4095) & ~static_cast<std::size_t>(4095));
                return again;
            }
    LPVOID result = oVirtualAlloc(address, size, type | (g_tracker.writeWatch ? MEM_WRITE_WATCH : 0), protect);
    if (!result && g_tracker.writeWatch && GetLastError() == ERROR_INVALID_PARAMETER) {
        // Write watching isn't available (an old Wine?): track without it
        result = oVirtualAlloc(address, size, type, protect);
        if (result) g_tracker.writeWatch = false;
    }
    if (result) {
        const std::size_t pages = (size + 4095) & ~static_cast<std::size_t>(4095);
        Insert(reinterpret_cast<std::uintptr_t>(result), pages);
    }
    return result;
}

BOOL WINAPI VirtualFreeHook(LPVOID address, SIZE_T size, DWORD type)
{
    if ((type & MEM_DECOMMIT) && FromGame(_ReturnAddress()))
        if (auto hold = g_tracker.holdDecommit; hold && hold(reinterpret_cast<std::uintptr_t>(address), size)) return TRUE;
    if (!(type & MEM_RELEASE) || !FromGame(_ReturnAddress())) return oVirtualFree(address, size, type);
    const auto base = reinterpret_cast<std::uintptr_t>(address);
    if (!Contains(base)) return oVirtualFree(address, size, type);
    if (auto hold = g_tracker.holdFree; hold && hold(base)) return TRUE;
    const BOOL ok = oVirtualFree(address, size, type);
    if (ok) Remove(base);
    return ok;
}

BOOL WINAPI CloseGameHandle(HANDLE handle) { return oCloseHandle(handle); }

DWORD WINAPI TlsAllocHook()
{
    const DWORD index = oTlsAlloc();
    if (index < kTlsIndices) g_tlsOwners[index] = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    return index;
}

BOOL WINAPI TlsFreeHook(DWORD index)
{
    if (index < kTlsIndices) g_tlsOwners[index] = 0;
    return oTlsFree(index);
}

BOOL WINAPI CloseHandleHook(HANDLE handle)
{
    const bool game = FromGame(_ReturnAddress());
    if (auto hold = g_tracker.holdClose; hold && game && hold(handle)) return TRUE;
    return oCloseHandle(handle);
}

void WINAPI ForEach(void(WINAPI* fn)(std::uintptr_t, std::size_t, void*), void* ctx)
{
    AcquireSRWLockShared(&g_lock);
    for (const Slot& s : g_slots)
        if (s.base) fn(s.base, s.size, ctx);
    ReleaseSRWLockShared(&g_lock);
}

BOOL WINAPI Release(std::uintptr_t base)
{
    const BOOL ok = oVirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
    Remove(base);
    return ok;
}

std::uint32_t WINAPI Count()
{
    AcquireSRWLockShared(&g_lock);
    const std::uint32_t n = g_count;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

std::uint64_t WINAPI Sequence()
{
    AcquireSRWLockShared(&g_lock);
    const std::uint64_t n = g_sequence;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

} // namespace

// Called from DllMain (process attach, only thread running): hooks kernelbase's VirtualAlloc / VirtualFree /
// CloseHandle. False if MinHook failed; the game then runs untracked (the menu's snapshots are unavailable).
bool Install()
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(dos) + dos->e_lfanew);
    g_tracker.size = sizeof(Tracker);
    g_tracker.exeBase = reinterpret_cast<std::uintptr_t>(dos);
    g_tracker.exeSize = nt->OptionalHeader.SizeOfImage;
    g_tracker.writeWatch = true;

    if (MH_Initialize() != MH_OK) return false;
    const wchar_t* dll = GetModuleHandleW(L"kernelbase.dll") ? L"kernelbase.dll" : L"kernel32.dll";
    if (MH_CreateHookApi(dll, "VirtualAlloc", reinterpret_cast<void*>(&VirtualAllocHook),
                         reinterpret_cast<void**>(&oVirtualAlloc)) != MH_OK ||
        MH_CreateHookApi(dll, "VirtualFree", reinterpret_cast<void*>(&VirtualFreeHook),
                         reinterpret_cast<void**>(&oVirtualFree)) != MH_OK ||
        MH_CreateHookApi(dll, "CloseHandle", reinterpret_cast<void*>(&CloseHandleHook),
                         reinterpret_cast<void**>(&oCloseHandle)) != MH_OK ||
        MH_CreateHookApi(dll, "TlsAlloc", reinterpret_cast<void*>(&TlsAllocHook),
                         reinterpret_cast<void**>(&oTlsAlloc)) != MH_OK ||
        MH_CreateHookApi(dll, "TlsFree", reinterpret_cast<void*>(&TlsFreeHook),
                         reinterpret_cast<void**>(&oTlsFree)) != MH_OK ||

        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_RemoveHook(MH_ALL_HOOKS);
        return false;
    }
    g_tracker.VirtualAlloc = oVirtualAlloc;
    g_tracker.VirtualFree = oVirtualFree;
    g_tracker.CloseHandle = oCloseHandle;
    g_tracker.ForEach = &ForEach;
    g_tracker.Release = &Release;
    g_tracker.Count = &Count;
    g_tracker.Sequence = &Sequence;
    g_tracker.CloseGameHandle = &CloseGameHandle;
    g_tracker.tlsOwners = g_tlsOwners;
    g_tracker.tlsIndices = kTlsIndices;
    g_tracker.version = kVersion;       // Last: Get() hands it out from now on
    return true;
}

Tracker* Get() { return g_tracker.version ? &g_tracker : nullptr; }

} // namespace memenu_tracker

extern "C" memenu_tracker::Tracker* WINAPI Proxy_MemenuGetTracker() { return memenu_tracker::Get(); }
