#include "tas/memory_map.h"
#include "../../loader/src/tracker.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <winternl.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace tas::memmap {

namespace {

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Narrow(const wchar_t* w)
{
    std::string out;
    for (; w && *w; ++w) out += static_cast<char>(*w < 128 ? *w : '?');
    return out;
}

std::string GameDir()
{
    static const std::string dir = [] {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::string p = Lower(Narrow(exe));
        p.erase(p.find_last_of('\\') + 1);
        return p;
    }();
    return dir;
}

// ---- ntdll -------------------------------------------------------------------------------------------------------------
using NtQueryInformationThread_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQuerySystemInformation_t = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

template <class T>
T Ntdll(const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
}

struct ThreadBasicInformation {
    LONG exitStatus;
    PVOID tebBaseAddress;
    CLIENT_ID clientId;
    ULONG_PTR affinityMask;
    LONG priority;
    LONG basePriority;
};

struct SystemThreadInformation {         // SYSTEM_THREAD_INFORMATION
    LARGE_INTEGER kernelTime, userTime, createTime;
    ULONG waitTime;
    PVOID startAddress;
    CLIENT_ID clientId;
    LONG priority;
    LONG basePriority;
    ULONG contextSwitches;
    ULONG threadState;
    ULONG waitReason;
};

// ---- allocation tracker ------------------------------------------------------------------------------------------------
using VirtualAlloc_t = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFree_t = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
using HeapCreate_t = HANDLE(WINAPI*)(DWORD, SIZE_T, SIZE_T);
using HeapDestroy_t = BOOL(WINAPI*)(HANDLE);

VirtualAlloc_t oVirtualAlloc = nullptr;
VirtualFree_t oVirtualFree = nullptr;
HeapCreate_t oHeapCreate = nullptr;
HeapDestroy_t oHeapDestroy = nullptr;
std::atomic<bool> g_tracking{false};
memenu_tracker::Tracker* g_loader = nullptr;     // the loader tracks the game's reservations from the start: use its table

memenu_tracker::Tracker* FindLoaderTracker()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    memenu_tracker::Tracker* found = nullptr;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        if (auto fn = reinterpret_cast<memenu_tracker::GetTracker_t>(GetProcAddress(me.hModule, memenu_tracker::kExport)))
            found = fn();
    CloseHandle(snap);
    if (found && (found->version != memenu_tracker::kVersion || found->size != sizeof(memenu_tracker::Tracker)))
        found = nullptr;
    return found;
}

struct Allocation {
    std::size_t size = 0;
    std::uint16_t module = 0;       // index into g_callerNames
};

std::shared_mutex g_allocMutex;
std::map<std::uintptr_t, Allocation> g_allocs;           // reservation base -> allocation (game and others)
std::unordered_map<HMODULE, std::uint16_t> g_callerIndex;
std::vector<std::string> g_callerNames;                  // [0] = "?"
std::vector<bool> g_callerGame;
std::vector<AllocStats> g_callerStats;
std::unordered_map<HANDLE, std::uint16_t> g_heaps;       // created heaps -> caller module
thread_local bool t_inHook = false;

std::uint16_t CallerIndex(const void* address)        // under g_allocMutex (exclusive)
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    if (g_callerNames.empty()) {
        g_callerNames.push_back("?");
        g_callerGame.push_back(false);
        g_callerStats.push_back({"?"});
    }
    if (!m) return 0;
    if (auto it = g_callerIndex.find(m); it != g_callerIndex.end()) return it->second;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);
    const std::string p = Lower(Narrow(path));
    const std::string name = p.substr(p.find_last_of('\\') + 1);
    const auto index = static_cast<std::uint16_t>(g_callerNames.size());
    g_callerNames.push_back(name);
    g_callerGame.push_back(IsGameModule(name, p));
    g_callerStats.push_back({name});
    g_callerIndex.emplace(m, index);
    return index;
}

LPVOID WINAPI VirtualAllocHook(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    void* const caller = _ReturnAddress();
    LPVOID result = oVirtualAlloc(address, size, type, protect);
    if (!result || t_inHook || !g_tracking.load(std::memory_order_relaxed)) return result;
    t_inHook = true;
    {
        std::unique_lock lock(g_allocMutex);
        const auto at = reinterpret_cast<std::uintptr_t>(result);
        // A commit inside a reservation we know already belongs to it
        auto it = g_allocs.upper_bound(at);
        const bool inside = it != g_allocs.begin() && (--it, at < it->first + it->second.size);
        if (!inside) {
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(result, &mbi, sizeof mbi);
            const auto base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase ? mbi.AllocationBase : result);
            // The reservation's whole size: walk the regions of the same allocation
            std::size_t total = 0;
            for (auto p = base;;) {
                MEMORY_BASIC_INFORMATION r{};
                if (!VirtualQuery(reinterpret_cast<void*>(p), &r, sizeof r) ||
                    reinterpret_cast<std::uintptr_t>(r.AllocationBase) != base)
                    break;
                total += r.RegionSize;
                p += r.RegionSize;
            }
            const std::uint16_t who = CallerIndex(caller);
            g_allocs[base] = {std::max<std::size_t>(total, size), who};
            auto& st = g_callerStats[who];
            ++st.calls;
            st.bytes += size;
            st.live += total;
        } else {
            ++g_callerStats[it->second.module].calls;
            g_callerStats[it->second.module].bytes += size;
        }
    }
    t_inHook = false;
    return result;
}

BOOL WINAPI VirtualFreeHook(LPVOID address, SIZE_T size, DWORD type)
{
    const BOOL ok = oVirtualFree(address, size, type);
    if (ok && (type & MEM_RELEASE) && !t_inHook && g_tracking.load(std::memory_order_relaxed)) {
        t_inHook = true;
        {
            std::unique_lock lock(g_allocMutex);
            if (auto it = g_allocs.find(reinterpret_cast<std::uintptr_t>(address)); it != g_allocs.end()) {
                auto& st = g_callerStats[it->second.module];
                st.live -= std::min<std::uint64_t>(st.live, it->second.size);
                g_allocs.erase(it);
            }
        }
        t_inHook = false;
    }
    return ok;
}

HANDLE WINAPI HeapCreateHook(DWORD options, SIZE_T initial, SIZE_T maximum)
{
    void* const caller = _ReturnAddress();
    HANDLE h = oHeapCreate(options, initial, maximum);
    if (h && !t_inHook && g_tracking.load(std::memory_order_relaxed)) {
        t_inHook = true;
        {
            std::unique_lock lock(g_allocMutex);
            g_heaps[h] = CallerIndex(caller);
        }
        t_inHook = false;
    }
    return h;
}

BOOL WINAPI HeapDestroyHook(HANDLE heap)
{
    if (!t_inHook && g_tracking.load(std::memory_order_relaxed)) {
        t_inHook = true;
        {
            std::unique_lock lock(g_allocMutex);
            g_heaps.erase(heap);
        }
        t_inHook = false;
    }
    return oHeapDestroy(heap);
}

bool HookExport(const wchar_t* module, const char* name, void* detour, void** original)
{
    HMODULE m = GetModuleHandleW(module);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    if (!target) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        spdlog::error("TAS memory: could not hook {}", name);
        return false;
    }
    return true;
}

// The heap msvcr80's malloc uses (shared by the game and the DLLs that link msvcr80).
HANDLE CrtHeap()
{
    HMODULE crt = GetModuleHandleW(L"msvcr80.dll");
    using GetHeap_t = intptr_t(__cdecl*)();
    auto fn = crt ? reinterpret_cast<GetHeap_t>(GetProcAddress(crt, "_get_heap_handle")) : nullptr;
    return fn ? reinterpret_cast<HANDLE>(fn()) : nullptr;
}

bool Writable(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD base = protect & 0xFF;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

struct Range {
    std::uintptr_t lo, hi;
    std::string what;
    Owner owner;
};

// The game module whose writable data holds `heap`'s handle ("" if none).
std::string HeapReferencedBy(HANDLE heap)
{
    const auto value = reinterpret_cast<std::uintptr_t>(heap);
    for (const auto& m : Modules()) {
        if (!m.game) continue;
        for (std::uintptr_t p = m.base; p < m.base + m.size;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
            const auto lo = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto hi = std::min<std::uintptr_t>(lo + mbi.RegionSize, m.base + m.size);
            if (mbi.State == MEM_COMMIT && Writable(mbi.Protect))
                for (auto q = lo; q + sizeof value <= hi; q += sizeof value)
                    if (*reinterpret_cast<const std::uintptr_t*>(q) == value) return m.name;
            p = hi;
        }
    }
    return {};
}

} // namespace

const char* OwnerName(Owner o)
{
    switch (o) {
        case Owner::GameImage:   return "game image";
        case Owner::GameHeap:    return "game heap";
        case Owner::GameVirtual: return "game virtual";
        case Owner::Stack:       return "stack";
        default:                 return "other";
    }
}

bool IsGameModule(const std::string& name, const std::string& path)
{
    if (name == "msvcr80.dll" || name == "msvcp80.dll") return true;          // the game's CRT (WinSxS)
    if (name.rfind("physx", 0) == 0 || name.rfind("nx", 0) == 0) return true; // PhysX, wherever it is installed
    const std::string dir = GameDir();
    if (path.compare(0, dir.size(), dir) != 0) return false;
    if (path.find("\\memenu\\") != std::string::npos) return false;           // the menu and its files
    if (name == "dinput8.dll") return false;                                   // the menu's loader
    // In Binaries, but not game state: sound (driven by its own threads), video, the editor's UI, the debug portal
    if (name == "wrap_oal.dll" || name == "openal32.dll" || name == "binkw32.dll" || name.rfind("wxmsw", 0) == 0 ||
        name == "dmcmdportalclient.dll")
        return false;
    return true;
}

std::vector<std::string> StackSample(std::uint32_t threadId, std::size_t max)
{
    std::vector<std::string> out;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!h) return out;
    const auto modules = Modules();
    const auto where = [&](std::uintptr_t a) -> std::string {
        for (const auto& m : modules)
            if (a >= m.base && a < m.base + m.size) {
                char text[96];
                std::snprintf(text, sizeof text, "%s+%#zx", m.name.c_str(), static_cast<std::size_t>(a - m.base));
                return text;
            }
        return {};
    };
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &ctx)) {
#ifdef _WIN64
            const std::uintptr_t ip = ctx.Rip, sp = ctx.Rsp;
#else
            const std::uintptr_t ip = ctx.Eip, sp = ctx.Esp;
#endif
            out.push_back("ip " + where(ip));
            // Values on the stack that point into a module: likely return addresses (no symbols, no frame walk)
            for (std::uintptr_t p = sp; p < sp + 16 * 1024 && out.size() < max; p += sizeof(void*)) {
                std::uintptr_t v = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), &v, sizeof v, nullptr)) break;
                if (std::string w = where(v); !w.empty() && w.find("medge_menu") == std::string::npos) out.push_back(w);
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return out;
}

// psapi, not a toolhelp module snapshot: that takes the loader lock, and a thread starting or ending (the rendering
// thread, the snapshot workers) can hold it at the start of a frame. psapi reads the module list without it.
std::vector<Module> Modules()
{
    std::vector<Module> out;
    std::vector<HMODULE> handles(512);
    DWORD needed = 0;
    const HANDLE self = GetCurrentProcess();
    if (!K32EnumProcessModulesEx(self, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed,
                                 LIST_MODULES_ALL))
        return out;
    if (needed > handles.size() * sizeof(HMODULE)) {
        handles.resize(needed / sizeof(HMODULE) + 16);
        if (!K32EnumProcessModulesEx(self, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed,
                                     LIST_MODULES_ALL))
            return out;
    }
    handles.resize(std::min<std::size_t>(handles.size(), needed / sizeof(HMODULE)));
    for (HMODULE h : handles) {
        MODULEINFO mi{};
        wchar_t path[MAX_PATH]{};
        if (!K32GetModuleInformation(self, h, &mi, sizeof mi)) continue;
        K32GetModuleFileNameExW(self, h, path, MAX_PATH);
        Module m;
        m.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
        m.size = mi.SizeOfImage;
        m.path = Lower(Narrow(path));
        m.name = m.path.substr(m.path.find_last_of('\\') + 1);
        m.game = IsGameModule(m.name, m.path);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    return out;
}

std::vector<Heap> Heaps()
{
    std::vector<Heap> out;
    std::vector<HANDLE> handles(256);
    DWORD n = GetProcessHeaps(static_cast<DWORD>(handles.size()), handles.data());
    if (n > handles.size()) {
        handles.resize(n);
        n = GetProcessHeaps(n, handles.data());
    }
    handles.resize(std::min<std::size_t>(n, handles.size()));
    const HANDLE crt = CrtHeap();
    const HANDLE process = GetProcessHeap();
    std::shared_lock lock(g_allocMutex);
    for (HANDLE h : handles) {
        Heap heap;
        heap.handle = h;
        if (h == crt) {
            heap.game = true;
            heap.what = "msvcr80 heap";
        } else if (h == process) {
            heap.what = "process heap";
        } else if (auto it = g_heaps.find(h); it != g_heaps.end()) {
            heap.game = g_callerGame[it->second];
            heap.what = "heap of " + g_callerNames[it->second];
        } else if (const std::string owner = HeapReferencedBy(h); !owner.empty()) {
            // Created before the tracker: a game module that keeps the handle in its data owns it (a DLL with its own
            // static CRT, like PhysXCore, keeps its heap in _crtheap)
            heap.game = true;
            heap.what = "heap of " + owner;
        } else {
            heap.what = "heap (unknown owner)";
        }
        out.push_back(std::move(heap));
    }
    return out;
}

std::vector<Thread> Threads()
{
    std::vector<Thread> out;
    static const auto queryThread = Ntdll<NtQueryInformationThread_t>("NtQueryInformationThread");
    static const auto querySystem = Ntdll<NtQuerySystemInformation_t>("NtQuerySystemInformation");
    const auto modules = Modules();
    const auto moduleOf = [&](std::uintptr_t a) -> std::string {
        for (const auto& m : modules)
            if (a >= m.base && a < m.base + m.size) return m.name;
        return "?";
    };

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        Thread t;
        t.id = te.th32ThreadID;
        if (HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, t.id)) {
            ThreadBasicInformation tbi{};
            if (queryThread && queryThread(h, 0, &tbi, sizeof tbi, nullptr) >= 0 && tbi.tebBaseAddress) {
                t.teb = reinterpret_cast<std::uintptr_t>(tbi.tebBaseAddress);
                const auto* tib = static_cast<const NT_TIB*>(tbi.tebBaseAddress);
                t.stackHigh = reinterpret_cast<std::uintptr_t>(tib->StackBase);
#ifdef _WIN64
                t.stackLow = *reinterpret_cast<const std::uintptr_t*>(t.teb + 0x1478);   // DeallocationStack
#else
                t.stackLow = *reinterpret_cast<const std::uintptr_t*>(t.teb + 0xE0C);
#endif
                if (!t.stackLow) t.stackLow = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
            }
            PVOID start = nullptr;
            if (queryThread && queryThread(h, 9, &start, sizeof start, nullptr) >= 0)    // Win32StartAddress
                t.start = reinterpret_cast<std::uintptr_t>(start);
            ULONG64 cycles = 0;
            if (QueryThreadCycleTime(h, &cycles)) t.cycles = cycles;
            CloseHandle(h);
        }
        t.startModule = moduleOf(t.start);
        out.push_back(std::move(t));
    }
    CloseHandle(snap);

    // Scheduler state of each thread
    if (querySystem) {
        std::vector<std::uint8_t> buffer(1 << 20);
        ULONG needed = 0;
        LONG st;
        while ((st = querySystem(5, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) ==
               static_cast<LONG>(0xC0000004))   // STATUS_INFO_LENGTH_MISMATCH
            buffer.resize(std::max<std::size_t>(buffer.size() * 2, needed + 65536));
        if (st >= 0) {
            for (std::size_t offset = 0;;) {
                const auto* p = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
                if (reinterpret_cast<std::uintptr_t>(p->UniqueProcessId) == GetCurrentProcessId()) {
                    const auto* threads = reinterpret_cast<const SystemThreadInformation*>(p + 1);
                    for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
                        const auto id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(threads[i].clientId.UniqueThread));
                        for (auto& t : out)
                            if (t.id == id) {
                                t.state = threads[i].threadState;
                                t.waitReason = threads[i].waitReason;
                            }
                    }
                    break;
                }
                if (!p->NextEntryOffset) break;
                offset += p->NextEntryOffset;
            }
        }
    }
    return out;
}

std::vector<Region> Regions()
{
    const auto modules = Modules();
    const auto threads = Threads();
    std::vector<Range> ranges;      // stacks and heap regions, sorted by address
    for (const auto& t : threads) {
        if (t.stackLow && t.stackHigh > t.stackLow)
            ranges.push_back({t.stackLow, t.stackHigh, "stack of thread " + std::to_string(t.id), Owner::Stack});
        if (t.teb) ranges.push_back({t.teb & ~std::uintptr_t(0xFFF), (t.teb & ~std::uintptr_t(0xFFF)) + 0x3000,
                                     "TEB of thread " + std::to_string(t.id), Owner::Stack});
    }
    for (const auto& heap : Heaps()) {
        const Owner owner = heap.game ? Owner::GameHeap : Owner::Other;
        if (!HeapLock(heap.handle)) continue;
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(heap.handle, &e)) {
            const auto data = reinterpret_cast<std::uintptr_t>(e.lpData);
            if (e.wFlags & PROCESS_HEAP_REGION) {
                const auto last = reinterpret_cast<std::uintptr_t>(e.Region.lpLastBlock);
                ranges.push_back({data, std::max<std::uintptr_t>(last, data + e.Region.dwCommittedSize), heap.what, owner});
            } else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                // Large blocks live in their own virtual allocation, outside the regions
                MEMORY_BASIC_INFORMATION mbi{};
                if (e.cbData >= 256 * 1024 && VirtualQuery(e.lpData, &mbi, sizeof mbi)) {
                    const auto base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
                    if (data - base < 0x1000)       // starts right after the block header: a heap "big block"
                        ranges.push_back({base, data + e.cbData, heap.what, owner});
                }
            }
        }
        HeapUnlock(heap.handle);
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });

    std::vector<std::pair<std::uintptr_t, std::size_t>> loaderAllocs;
    if (g_loader) {
        g_loader->ForEach([](std::uintptr_t base, std::size_t size, void* ctx) {
            static_cast<std::vector<std::pair<std::uintptr_t, std::size_t>>*>(ctx)->emplace_back(base, size);
        }, &loaderAllocs);
        std::sort(loaderAllocs.begin(), loaderAllocs.end());
    }

    std::vector<Region> out;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto p = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    std::shared_lock lock(g_allocMutex);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::size_t size = mbi.RegionSize;
        p = base + size;
        if (mbi.State != MEM_COMMIT || !Writable(mbi.Protect)) continue;

        Region r;
        r.base = base;
        r.size = size;
        r.protect = mbi.Protect;
        r.type = mbi.Type;
        if (mbi.Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) {
            r.what = "uncached (driver)";
        } else if (mbi.Type == MEM_IMAGE) {
            const auto alloc = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
            for (const auto& m : modules)
                if (m.base == alloc) {
                    r.what = m.name;
                    if (m.game) r.owner = Owner::GameImage;
                }
        } else if (mbi.Type == MEM_MAPPED) {
            r.what = "mapped";
        } else {
            // Stack / heap ranges overlapping this region (the first one that covers its start decides)
            for (const auto& range : ranges) {
                if (range.lo > base) break;
                if (base < range.hi) {
                    r.owner = range.owner;
                    r.what = range.what;
                }
            }
            if (r.what.empty()) {
                const auto alloc = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
                const auto la = std::lower_bound(loaderAllocs.begin(), loaderAllocs.end(), std::make_pair(alloc, std::size_t{0}));
                if (la != loaderAllocs.end() && la->first == alloc) {
                    r.what = "virtual of mirrorsedge.exe (write watched)";
                    r.owner = Owner::GameVirtual;
                } else if (auto it = g_allocs.find(alloc); it != g_allocs.end()) {
                    r.what = "virtual of " + g_callerNames[it->second.module];
                    if (g_callerGame[it->second.module]) r.owner = Owner::GameVirtual;
                } else {
                    r.what = "virtual (unknown)";
                }
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

namespace {
std::uint64_t HashWords(const std::uint64_t* w)
{
    // Four independent lanes so the multiplies overlap; good enough to tell a changed page from an unchanged one.
    std::uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    for (std::size_t i = 0; i < kPage / 8; i += 4) {
        a = (a ^ w[i]) * 0x100000001B3ull;
        b = (b ^ w[i + 1]) * 0x100000001B3ull;
        c = (c ^ w[i + 2]) * 0x100000001B3ull;
        d = (d ^ w[i + 3]) * 0x100000001B3ull;
    }
    return a ^ (b << 1 | b >> 63) ^ (c << 2 | c >> 62) ^ (d << 3 | d >> 61);
}
} // namespace

bool HashPage(const void* page, std::uint64_t& hash)
{
    __try {
        hash = HashWords(static_cast<const std::uint64_t*>(page));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Install()
{
    if (g_tracking) return true;
    if ((g_loader = FindLoaderTracker())) {
        g_tracking = true;
        spdlog::info("TAS memory: the loader tracks the game's allocations ({} reservations)", g_loader->Count());
        return true;
    }
    // MinHook is initialised by hooks::Install() before the TAS starts.
    const bool k = GetModuleHandleW(L"kernelbase.dll") != nullptr;
    const wchar_t* dll = k ? L"kernelbase.dll" : L"kernel32.dll";
    bool ok = HookExport(dll, "VirtualAlloc", reinterpret_cast<void*>(&VirtualAllocHook), reinterpret_cast<void**>(&oVirtualAlloc)) &&
              HookExport(dll, "VirtualFree", reinterpret_cast<void*>(&VirtualFreeHook), reinterpret_cast<void**>(&oVirtualFree)) &&
              HookExport(dll, "HeapCreate", reinterpret_cast<void*>(&HeapCreateHook), reinterpret_cast<void**>(&oHeapCreate)) &&
              HookExport(dll, "HeapDestroy", reinterpret_cast<void*>(&HeapDestroyHook), reinterpret_cast<void**>(&oHeapDestroy));
    if (!ok) {
        spdlog::error("TAS memory: the allocation tracker could not be installed");
        return false;
    }
    g_tracking = true;
    spdlog::info("TAS memory: tracking the game's allocations");
    return true;
}

bool Tracking() { return g_tracking; }

std::vector<AllocStats> AllocationCallers()
{
    std::shared_lock lock(g_allocMutex);
    auto out = g_callerStats;
    std::sort(out.begin(), out.end(), [](const AllocStats& a, const AllocStats& b) { return a.bytes > b.bytes; });
    return out;
}

} // namespace tas::memmap
