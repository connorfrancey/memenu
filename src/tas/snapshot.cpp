#include "tas/snapshot.h"
#include "tas/d3d_pins.h"
#include "tas/engine.h"
#include "tas/memory_map.h"

#include "../../loader/src/tracker.h"
#include "game/game.h"
#include "diag.h"
#include "paths.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <winternl.h>

#include <spdlog/spdlog.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tas::snapshot {

namespace {

using memmap::kPage;

double NowMs() { return engine::RealSeconds() * 1000.0; }

memenu_tracker::Tracker* g_tracker = nullptr;
std::string g_why = "not initialised";

// ---- page store: compressed pages in page file backed sections, mapped only while read or written ----------------------
struct Blob {
    std::uint32_t section = 0, offset = 0, size = 0;    // size 0: a page of zeros
};

class Store {
public:
    ~Store() { Clear(); }

    Blob Put(const void* data, std::uint32_t size)
    {
        if (size == 0) return {};
        std::lock_guard lock(mutex_);
        if (sections_.empty() || sections_.back().used + size > kSection) {
            if (!NewSection()) return {0, 0, 0xFFFFFFFFu};
        }
        Section& s = sections_.back();
        std::uint8_t* view = Map(static_cast<std::uint32_t>(sections_.size() - 1));
        if (!view) return {0, 0, 0xFFFFFFFFu};
        std::memcpy(view + s.used, data, size);
        Blob b{static_cast<std::uint32_t>(sections_.size() - 1), s.used, size};
        s.used += (size + 7) & ~7u;
        s.live += size;
        bytes_ += size;
        return b;
    }

    // The blob's bytes; valid until the next Put/Get on this thread (views are recycled).
    const std::uint8_t* Get(const Blob& b)
    {
        std::lock_guard lock(mutex_);
        std::uint8_t* view = Map(b.section);
        return view ? view + b.offset : nullptr;
    }

    void Drop(const Blob& b)
    {
        if (b.size == 0 || b.size == 0xFFFFFFFFu) return;
        std::lock_guard lock(mutex_);
        Section& s = sections_[b.section];
        s.live -= std::min(s.live, b.size);
        bytes_ -= std::min<std::uint64_t>(bytes_, b.size);
        if (s.live == 0 && b.section + 1 != sections_.size() && s.handle) {
            Unmap(b.section);
            CloseHandle(s.handle);
            s.handle = nullptr;
        }
    }

    void Clear()
    {
        std::lock_guard lock(mutex_);
        for (std::uint32_t i = 0; i < sections_.size(); ++i) {
            Unmap(i);
            if (sections_[i].handle) CloseHandle(sections_[i].handle);
        }
        sections_.clear();
        bytes_ = 0;
    }

    std::uint64_t Bytes() const { return bytes_; }

private:
    static constexpr std::uint32_t kSection = 32u << 20;
    static constexpr int kViews = 6;
    struct Section {
        HANDLE handle = nullptr;
        std::uint32_t used = 0, live = 0;
    };
    struct View {
        std::uint32_t section = 0xFFFFFFFFu;
        std::uint8_t* p = nullptr;
        std::uint64_t used = 0;
    };

    bool NewSection()
    {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kSection, nullptr);
        if (!h) {
            spdlog::error("TAS snapshots: no memory for more snapshots (error {})", GetLastError());
            return false;
        }
        sections_.push_back({h, 0, 0});
        return true;
    }

    std::uint8_t* Map(std::uint32_t section)
    {
        ++tick_;
        for (View& v : views_)
            if (v.section == section && v.p) {
                v.used = tick_;
                return v.p;
            }
        View* victim = &views_[0];
        for (View& v : views_)
            if (!v.p || v.used < victim->used) victim = &v;
        if (victim->p) UnmapViewOfFile(victim->p);
        victim->p = static_cast<std::uint8_t*>(MapViewOfFile(sections_[section].handle, FILE_MAP_ALL_ACCESS, 0, 0, kSection));
        victim->section = victim->p ? section : 0xFFFFFFFFu;
        victim->used = tick_;
        return victim->p;
    }

    void Unmap(std::uint32_t section)
    {
        for (View& v : views_)
            if (v.section == section && v.p) {
                UnmapViewOfFile(v.p);
                v = {};
            }
    }

    std::mutex mutex_;
    std::vector<Section> sections_;
    View views_[kViews];
    std::uint64_t tick_ = 0;
    std::uint64_t bytes_ = 0;
};

// ---- session state --------------------------------------------------------------------------------------------------------
struct Version {
    std::uint32_t frame;
    Blob blob;
};

// A TLS slot of one of the game's threads, for an index a game module allocated (PhysX keeps per-thread data there)
struct TlsValue {
    std::uint32_t thread;
    std::uint32_t index;
    std::uintptr_t value;
};

struct Keyframe {
    std::vector<TlsValue> tls;
    // The engine's frames on the game thread's stack (its main loop, above the frame timing call): its locals are game
    // state too. Put back only when the call comes from the same place.
    std::uintptr_t stackFrom = 0;
    std::vector<std::uint8_t> stack;
};

void CaptureStack(Keyframe& key)
{
    const std::uintptr_t from = engine::CallerStack();
    const auto top = reinterpret_cast<std::uintptr_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    key.stackFrom = 0;
    key.stack.clear();
    if (!from || from >= top || top - from > (8u << 20)) return;
    key.stackFrom = from;
    key.stack.assign(reinterpret_cast<const std::uint8_t*>(from), reinterpret_cast<const std::uint8_t*>(top));
}

bool RestoreStack(const Keyframe& key)
{
    if (!key.stackFrom || key.stackFrom != engine::CallerStack()) return false;
    std::memcpy(reinterpret_cast<void*>(key.stackFrom), key.stack.data(), key.stack.size());
    return true;
}

struct Region {
    std::uintptr_t base;
    std::size_t size;
};


std::atomic<bool> g_active{false};
std::uint32_t g_frame = 0;                  // the frame running now
Store g_store;
std::unordered_map<std::uintptr_t, std::vector<Version>> g_pages;
std::map<std::uint32_t, Keyframe> g_keys;
// The pages that have a version stored with each frame: what changed up to that frame since the version before. Kept by
// version, not by keyframe: a keyframe thinned out leaves the versions the next one still needs, under their own frame.
std::map<std::uint32_t, std::vector<std::uintptr_t>> g_versionPages;
std::uint64_t g_budget = 2048ull << 20;     // stored bytes before keyframes are thinned out
std::optional<std::uint32_t> g_anchor;

// Memory without write watching: the game's modules' data and its heaps, compared against what they held at the anchor
std::vector<Region> g_unwatched;
std::unordered_map<std::uintptr_t, std::unique_ptr<std::uint8_t[]>> g_shadow;

// ---- timelines ----
// What the game does with its reservations and handles during a session, by frame: the new reservations it makes (the
// game thread's), the ones it releases and the handles it closes. Releases and closes are held back (see tracker.h): an
// older state may still use them.
//
// Two timelines: the recorded one, which the keyframes hold, and the run since the last restore or snap (g_runFrom).
// The game is in "recorded before g_runFrom, then the run". A restore or a snap to keyframe k makes it "recorded before
// k" again (the run's own reservations are nobody's then); a new keyframe makes the run part of the recorded timeline.
// When the session ends, what that final timeline released or closed is let go of for real, and the session's
// reservations it doesn't hold are released.
enum class EventKind : std::uint8_t { Alloc, Free, Close };
struct Event {
    std::uint32_t frame;
    EventKind kind;
    std::uintptr_t what;                    // a reservation's base, or a handle
    SIZE_T size;                            // Alloc: what the game asked for
    DWORD type, protect;
};
std::mutex g_eventMutex;
std::vector<Event> g_recorded, g_run;
std::uint32_t g_runFrom = 0;
std::deque<Event> g_replay;                 // the recorded timeline's reservations from the last restore on, in order
std::unordered_set<std::uintptr_t> g_sessionAllocs;   // every reservation handed to the game thread during the session

std::vector<void (*)()> g_onRestore;


// What the last session let go of when it ended, for the crash log ("the address is in ...")
struct Released {
    std::uintptr_t base;
    std::size_t size;
    const char* why;
};
std::mutex g_releasedMutex;
std::vector<Released> g_released;

std::string DescribeReleased(std::uintptr_t address)
{
    std::lock_guard lock(g_releasedMutex);
    for (const Released& r : g_released)
        if (address - r.base < r.size) {
            char text[160];
            std::snprintf(text, sizeof text, "a reservation the TAS snapshots released when the session ended (%s, %p+%zu KB)",
                          r.why, reinterpret_cast<void*>(r.base), r.size / 1024);
            return text;
        }
    return {};
}

std::size_t ReservationSize(std::uintptr_t base)
{
    std::size_t size = 0;
    for (std::uintptr_t p = base;;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) ||
            reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) != base || mbi.State == MEM_FREE)
            break;
        size += mbi.RegionSize;
        p += mbi.RegionSize;
    }
    return size;
}
std::mutex g_statsMutex;
Stats g_stats;

ZSTD_CCtx* g_cctx = nullptr;
ZSTD_DCtx* g_dctx = nullptr;
constexpr int kLevel = 1;

void SetError(std::string text)
{
    spdlog::error("TAS snapshots: {}", text);
    std::lock_guard lock(g_statsMutex);
    g_stats.lastError = std::move(text);
}

// ---- page access (SEH: no C++ objects in these) --------------------------------------------------------------------------
bool CopyPage(void* dst, const void* src)
{
    __try {
        std::memcpy(dst, src, kPage);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SamePage(const void* a, const void* b, bool& same)
{
    __try {
        same = std::memcmp(a, b, kPage) == 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ZeroPage(void* dst)
{
    __try {
        std::memset(dst, 0, kPage);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DecompressInto(ZSTD_DCtx* dctx, void* dst, const void* src, std::size_t size)
{
    __try {
        const std::size_t n = ZSTD_decompressDCtx(dctx, dst, kPage, src, size);
        return !ZSTD_isError(n) && n == kPage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsZero(const std::uint8_t* p)
{
    const auto* w = reinterpret_cast<const std::uint64_t*>(p);
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < kPage / 8; ++i) acc |= w[i];
    return acc == 0;
}

bool Writable(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD base = protect & 0xFF;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

// ---- the game's memory ------------------------------------------------------------------------------------------------------
std::vector<Region> Watched()
{
    std::vector<Region> out;
    out.reserve(g_tracker->Count() + 64);
    g_tracker->ForEach(
        [](std::uintptr_t base, std::size_t size, void* ctx) {
            static_cast<std::vector<Region>*>(ctx)->push_back({base, size});
        },
        &out);
    std::sort(out.begin(), out.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
    return out;
}

// Found when a session begins: the game modules' address ranges and the game's heaps.
std::vector<Region> g_gameModules;
std::vector<HANDLE> g_gameHeaps;

void FindGameModulesAndHeaps()
{
    g_gameModules.clear();
    g_gameHeaps.clear();
    for (const auto& m : memmap::Modules())
        if (m.game) g_gameModules.push_back({m.base, m.size});
    for (const auto& heap : memmap::Heaps())
        if (heap.game) g_gameHeaps.push_back(heap.handle);
}

// The game modules' writable data (not code: hooks patch code, and code isn't state) and the game's heaps.
std::vector<Region> Unwatched()
{
    std::vector<Region> out;
    const auto addRange = [&](std::uintptr_t lo, std::uintptr_t hi, bool image) {
        for (std::uintptr_t p = lo; p < hi;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
            const auto b = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto e = std::min<std::uintptr_t>(b + mbi.RegionSize, hi);
            const bool exec = (mbi.Protect & 0xF0) != 0;
            if (mbi.State == MEM_COMMIT && Writable(mbi.Protect) && !exec && (!image || mbi.Type == MEM_IMAGE))
                out.push_back({std::max(b, p), e - std::max(b, p)});
            p = e;
        }
    };
    for (const Region& m : g_gameModules) addRange(m.base, m.base + m.size, true);
    for (HANDLE heap : g_gameHeaps) {
        if (!HeapLock(heap)) continue;
        PROCESS_HEAP_ENTRY e{};
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;
        while (HeapWalk(heap, &e)) {
            const auto data = reinterpret_cast<std::uintptr_t>(e.lpData);
            if (e.wFlags & PROCESS_HEAP_REGION) {
                ranges.emplace_back(data & ~(kPage - 1), reinterpret_cast<std::uintptr_t>(e.Region.lpLastBlock));
            } else if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && e.cbData >= 256 * 1024) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(e.lpData, &mbi, sizeof mbi)) {
                    const auto base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
                    if (data - base < 0x1000) ranges.emplace_back(base, data + e.cbData);
                }
            }
        }
        HeapUnlock(heap);
        for (const auto& [lo, hi] : ranges) addRange(lo, (hi + kPage - 1) & ~(kPage - 1), false);
    }
    std::sort(out.begin(), out.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
    return out;
}

// Committed, readable pages of a range.
template <class F>
void ForCommittedPages(std::uintptr_t lo, std::uintptr_t hi, F&& f)
{
    for (std::uintptr_t p = lo; p < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
        const auto b = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto e = std::min<std::uintptr_t>(b + mbi.RegionSize, hi);
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            for (std::uintptr_t q = std::max(b, p); q < e; q += kPage) f(q);
        p = e;
    }
}

// Pages written since the anchor (write watch for the game's reservations, comparison for the rest). With `reset`, the
// write watch starts over (the shadow of the unwatched pages is updated by the caller).
std::vector<std::uintptr_t> Dirty(const std::vector<Region>& watched, bool reset)
{
    std::vector<std::uintptr_t> out;
    static std::vector<PVOID> addresses;
    for (const Region& r : watched) {
        const std::size_t pages = r.size / kPage;
        if (addresses.size() < pages) addresses.resize(pages);
        ULONG_PTR count = pages;
        DWORD granularity = 0;
        if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(r.base), r.size, addresses.data(),
                          &count, &granularity) != 0)
            continue;   // not a write watched reservation any more (released by someone else)
        for (ULONG_PTR i = 0; i < count; ++i) out.push_back(reinterpret_cast<std::uintptr_t>(addresses[i]));
    }
    for (const Region& r : g_unwatched)
        for (std::uintptr_t p = r.base; p < r.base + r.size; p += kPage) {
            const auto it = g_shadow.find(p);
            bool same = false;
            if (it == g_shadow.end() || !SamePage(reinterpret_cast<const void*>(p), it->second.get(), same) || !same)
                out.push_back(p);
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void ResetWatch(const std::vector<Region>& watched)
{
    for (const Region& r : watched) ResetWriteWatch(reinterpret_cast<void*>(r.base), r.size);
}

// Write watch reset only in the reservations holding `pages` (sorted): after a restore, its own writes.
void ResetTouched(const std::vector<Region>& watched, const std::vector<std::uintptr_t>& pages)
{
    const Region* last = nullptr;
    for (std::uintptr_t p : pages) {
        if (last && p >= last->base && p < last->base + last->size) continue;
        auto it = std::upper_bound(watched.begin(), watched.end(), p, [](std::uintptr_t a, const Region& r) { return a < r.base; });
        if (it == watched.begin()) continue;
        const Region& r = *std::prev(it);
        if (p >= r.base + r.size) continue;
        ResetWriteWatch(reinterpret_cast<void*>(r.base), r.size);
        last = &r;
    }
}

bool IsUnwatched(std::uintptr_t page)
{
    auto it = std::upper_bound(g_unwatched.begin(), g_unwatched.end(), page,
                               [](std::uintptr_t p, const Region& r) { return p < r.base; });
    return it != g_unwatched.begin() && page < std::prev(it)->base + std::prev(it)->size;
}

void UpdateShadow(std::uintptr_t page)
{
    auto& slot = g_shadow[page];
    if (!slot) slot = std::make_unique<std::uint8_t[]>(kPage);
    CopyPage(slot.get(), reinterpret_cast<const void*>(page));
}

namespace memmap_detail {
std::string Narrow(const wchar_t* w)
{
    std::string out;
    for (; w && *w; ++w) out += static_cast<char>(*w < 128 ? *w : '?');
    return out;
}
} // namespace memmap_detail

// ---- threads ------------------------------------------------------------------------------------------------------------------
// The game's other threads (engine workers, PhysX) are suspended while a snapshot is taken or put back, at a moment each
// of them waits for work: none is in the middle of changing the game's memory.
//
// The list is found once a session (memmap::Threads walks every process: slow) and checked cheaply after that: a
// thread is idle when, suspended, it stands in one of ntdll's wait system calls.
struct GameThread {
    std::uint32_t id = 0;
    HANDLE handle = nullptr;
    std::uintptr_t teb = 0;
};
std::vector<GameThread> g_threads;
int g_threadsAge = 0;                       // captures since the list was made

struct Stub {
    std::uintptr_t lo, hi;
};
std::vector<Stub> g_waitStubs;              // the wait system calls (ntdll, win32u): an idle thread's instruction pointer

void FindWaitStubs()
{
    if (!g_waitStubs.empty()) return;
    const auto add = [](const wchar_t* dll, const char* name) {
        HMODULE m = GetModuleHandleW(dll);
        if (auto fn = m ? reinterpret_cast<std::uintptr_t>(GetProcAddress(m, name)) : 0) g_waitStubs.push_back({fn, fn + 32});
    };
    for (const char* name : {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtWaitForMultipleObjects32",
                             "NtDelayExecution", "NtRemoveIoCompletion", "NtRemoveIoCompletionEx", "NtWaitForAlertByThreadId",
                             "NtWaitForKeyedEvent", "NtSignalAndWaitForSingleObject", "NtWaitForWorkViaWorkerFactory",
                             "NtReplyWaitReceivePort", "NtAlertThreadByThreadIdEx", "NtYieldExecution"})
        add(L"ntdll.dll", name);
    for (const char* name : {"NtUserGetMessage", "NtUserMsgWaitForMultipleObjectsEx", "NtUserWaitMessage",
                             "NtUserPeekMessage"})
        add(L"win32u.dll", name);
}

void ForgetThreads()
{
    for (const GameThread& t : g_threads) CloseHandle(t.handle);
    g_threads.clear();
}

void FindGameThreads()
{
    ForgetThreads();
    FindWaitStubs();
    const DWORD self = GetCurrentThreadId();
    for (const auto& t : memmap::Threads()) {
        if (t.id == self) continue;
        const std::string& m = t.startModule;
        if (m != "mirrorsedge.exe" && m.rfind("physx", 0) != 0 && m != "msvcr80.dll") continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, t.id);
        if (h) g_threads.push_back({t.id, h, t.teb});
    }
    g_threadsAge = 0;
}

class Frozen {
public:
    explicit Frozen(double timeoutMs) { ok_ = Freeze(timeoutMs); }
    ~Frozen() { Thaw(); }
    bool ok() const { return ok_; }
    // The game's threads (this one included): id and TEB
    const std::vector<std::pair<std::uint32_t, std::uintptr_t>>& threads() const { return threads_; }

private:
    // Every one of `ids` is blocked in a wait right now (KTHREAD Waiting): none has a wait already satisfied (Ready to run
    // its next piece of work). Checked before suspending: a suspension interrupts the wait with its own.
    static bool Blocked(const std::vector<std::uint32_t>& ids)
    {
        static const auto query = reinterpret_cast<LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG)>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        static std::vector<std::uint8_t> buffer(1 << 20);
        ULONG needed = 0;
        LONG st;
        while ((st = query(5, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) == static_cast<LONG>(0xC0000004))
            buffer.resize(std::max<std::size_t>(buffer.size() * 2, needed + 65536));
        if (st < 0) return true;                          // can't tell: as before
        struct SystemThread {
            LARGE_INTEGER kernelTime, userTime, createTime;
            ULONG waitTime;
            PVOID startAddress;
            CLIENT_ID clientId;
            LONG priority, basePriority;
            ULONG contextSwitches, threadState, waitReason;
        };
        for (std::size_t offset = 0;;) {
            const auto* proc = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
            if (reinterpret_cast<std::uintptr_t>(proc->UniqueProcessId) == GetCurrentProcessId()) {
                const auto* threads = reinterpret_cast<const SystemThread*>(proc + 1);
                for (ULONG i = 0; i < proc->NumberOfThreads; ++i) {
                    const auto id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(threads[i].clientId.UniqueThread));
                    if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
                    if (threads[i].threadState != 5) {                  // 5 = Waiting
                        static int logged = 0;
                        if (logged++ < 20)
                            spdlog::info("TAS snapshots: thread {} not blocked (state {}, wait {}, start {})", id,
                                         threads[i].threadState, threads[i].waitReason, threads[i].startAddress);
                        return false;
                    }
                }
                return true;
            }
            if (!proc->NextEntryOffset) return true;
            offset += proc->NextEntryOffset;
        }
    }

    static bool Waiting(HANDLE h)
    {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(h, &ctx)) return false;
#ifdef _WIN64
        const std::uintptr_t ip = ctx.Rip;
#else
        const std::uintptr_t ip = ctx.Eip;
#endif
        for (const Stub& s : g_waitStubs)
            if (ip >= s.lo && ip < s.hi) return true;
        return false;
    }

    bool Freeze(double timeoutMs)
    {
        if (g_threads.empty() || ++g_threadsAge > 120) FindGameThreads();   // now and then: new threads (PhysX)
        const double end = NowMs() + timeoutMs;
        for (;;) {
            threads_.clear();
            threads_.emplace_back(GetCurrentThreadId(), reinterpret_cast<std::uintptr_t>(NtCurrentTeb()));
            // Blocked first, then suspended; a thread that ran in between (its cycle count moved) means another try
            std::vector<std::uint32_t> ids;
            std::vector<ULONG64> cycles(g_threads.size(), 0);
            for (std::size_t i = 0; i < g_threads.size(); ++i) {
                ids.push_back(g_threads[i].id);
                QueryThreadCycleTime(g_threads[i].handle, &cycles[i]);
            }
            bool idle = Blocked(ids);
            for (std::size_t i = 0; idle && i < g_threads.size(); ++i) {
                const GameThread& t = g_threads[i];
                if (WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0) continue;     // ended
                ULONG64 before = 0;
                if (QueryThreadCycleTime(t.handle, &before) && before != cycles[i]) {
                    idle = false;                               // it ran since it was seen blocked
                    break;
                }
                if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
                suspended_.push_back(t.handle);
                threads_.emplace_back(t.id, t.teb);
                if (!Waiting(t.handle)) {
                    idle = false;
                    break;
                }
            }
            // In a wait isn't enough: one whose wait is already satisfied (its event set, not run yet) is about to work on
            // something queued. A keyframe taken then holds the request queued while the event that wakes it for that
            // request is used up: put back, the request never runs (a level load waited for it forever). Blocked() above.
            if (idle) return true;
            Thaw();
            if (NowMs() > end) return false;
            Sleep(0);                                       // let the busy one get to its wait
        }
    }

    void Thaw()
    {
        for (HANDLE h : suspended_) ResumeThread(h);
        suspended_.clear();
    }

    bool ok_ = false;
    std::vector<HANDLE> suspended_;
    std::vector<std::pair<std::uint32_t, std::uintptr_t>> threads_;
};

// ---- TLS -------------------------------------------------------------------------------------------------------------------
bool GameTlsIndex(std::uint32_t index)
{
    const std::uintptr_t caller = g_tracker->tlsOwners[index];
    if (!caller) return false;
    for (const Region& m : g_gameModules)
        if (caller - m.base < m.size) return true;
    return false;
}

std::uintptr_t* TlsSlot(std::uintptr_t teb, std::uint32_t index)
{
    if (!teb) return nullptr;
#ifdef _WIN64
    constexpr std::uintptr_t kSlots = 0x1480, kExpansion = 0x1780;
#else
    constexpr std::uintptr_t kSlots = 0xE10, kExpansion = 0xF94;
#endif
    if (index < 64) return reinterpret_cast<std::uintptr_t*>(teb + kSlots) + index;
    auto* expansion = *reinterpret_cast<std::uintptr_t**>(teb + kExpansion);
    return expansion ? expansion + (index - 64) : nullptr;
}

bool ReadSlot(const std::uintptr_t* slot, std::uintptr_t& value)
{
    __try {
        value = *slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteSlot(std::uintptr_t* slot, std::uintptr_t value)
{
    __try {
        *slot = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's TLS slots of the game's threads (zeros too: a slot set after the keyframe must go back to empty)
std::vector<TlsValue> CaptureTls(const Frozen& frozen)
{
    std::vector<TlsValue> out;
    for (std::uint32_t i = 0; i < g_tracker->tlsIndices; ++i) {
        if (!GameTlsIndex(i)) continue;
        for (const auto& [id, teb] : frozen.threads()) {
            std::uintptr_t value = 0;
            if (std::uintptr_t* slot = TlsSlot(teb, i); slot && ReadSlot(slot, value)) out.push_back({id, i, value});
        }
    }
    return out;
}

void RestoreTls(const std::vector<TlsValue>& values, const Frozen& frozen)
{
    for (const TlsValue& v : values)
        for (const auto& [id, teb] : frozen.threads())
            if (id == v.thread)
                if (std::uintptr_t* slot = TlsSlot(teb, v.index)) WriteSlot(slot, v.value);
}

// ---- rendering thread -----------------------------------------------------------------------------------------------------
// UE3's rendering thread reads its commands with a position of its own that no snapshot can put back: running through a
// restore, it executed stale commands. So it is switched off (ToggleRenderingThread: the engine renders on the game
// thread, like the game's own -ONETHREAD) at the first session, and stays off until the game closes. Switched on again
// at the end of a session, the next session's switch off hung the engine.
int ExeThreads()
{
    int n = 0;
    for (const auto& t : memmap::Threads())
        if (t.startModule == "mirrorsedge.exe") ++n;
    return n;
}

bool g_renderingThreadOff = false;

void StopRenderingThread()
{
    if (g_renderingThreadOff) return;
    const int before = ExeThreads();
    if (!game::ConsoleCommand("ToggleRenderingThread")) return;
    if (ExeThreads() >= before) game::ConsoleCommand("ToggleRenderingThread");   // it was off already: leave it so
    g_renderingThreadOff = true;
    spdlog::info("TAS snapshots: the engine renders on the game thread from now on (rendering thread off)");
}

// ---- compression ------------------------------------------------------------------------------------------------------------
Blob StorePage(ZSTD_CCtx* cctx, const std::uint8_t* page, std::vector<std::uint8_t>& scratch)
{
    if (IsZero(page)) return {};
    scratch.resize(ZSTD_compressBound(kPage));
    const std::size_t n = ZSTD_compressCCtx(cctx, scratch.data(), scratch.size(), page, kPage, kLevel);
    if (ZSTD_isError(n) || n >= kPage) return g_store.Put(page, static_cast<std::uint32_t>(kPage));   // stored as is
    return g_store.Put(scratch.data(), static_cast<std::uint32_t>(n));
}

// The page's bytes in a version. False if the store lost it.
bool LoadVersion(const Blob& b, std::uint8_t* out)
{
    if (b.size == 0) {
        std::memset(out, 0, kPage);
        return true;
    }
    const std::uint8_t* data = g_store.Get(b);
    if (!data) return false;
    if (b.size == kPage) {
        std::memcpy(out, data, kPage);
        return true;
    }
    const std::size_t n = ZSTD_decompressDCtx(g_dctx, out, kPage, data, b.size);
    return !ZSTD_isError(n) && n == kPage;
}

const Version* VersionAt(std::uintptr_t page, std::uint32_t frame)
{
    const auto it = g_pages.find(page);
    if (it == g_pages.end()) return nullptr;
    const auto& v = it->second;
    auto at = std::upper_bound(v.begin(), v.end(), frame, [](std::uint32_t f, const Version& x) { return f < x.frame; });
    return at == v.begin() ? nullptr : &*std::prev(at);
}

// Pages with a version stored in (lo, hi]: the ones that differ between those two moments
void PagesBetween(std::uint32_t lo, std::uint32_t hi, std::vector<std::uintptr_t>& out)
{
    for (auto it = g_versionPages.upper_bound(lo); it != g_versionPages.end() && it->first <= hi; ++it)
        out.insert(out.end(), it->second.begin(), it->second.end());
}

// Writes a page back, committing (or reserving) its memory first if the game gave it back meanwhile.
bool WritePage(std::uintptr_t page, const std::uint8_t* data)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof mbi)) return false;
    if (mbi.State == MEM_FREE) {
        if (!g_tracker->VirtualAlloc(reinterpret_cast<void*>(page), kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return false;
    } else if (mbi.State == MEM_RESERVE) {
        if (!g_tracker->VirtualAlloc(reinterpret_cast<void*>(page), kPage, MEM_COMMIT, PAGE_READWRITE)) return false;
    } else if (!Writable(mbi.Protect)) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(page), kPage, PAGE_READWRITE, &old)) return false;
        const bool ok = CopyPage(reinterpret_cast<void*>(page), data);
        VirtualProtect(reinterpret_cast<void*>(page), kPage, old, &old);
        return ok;
    }
    return CopyPage(reinterpret_cast<void*>(page), data);
}

// ---- what the game lets go of ------------------------------------------------------------------------------------------------
BOOL WINAPI HoldFree(std::uintptr_t base)
{
    if (!g_active) return FALSE;        // Any of the game's threads: a released reservation would lose its write watch
    std::lock_guard lock(g_eventMutex);
    g_run.push_back({g_frame, EventKind::Free, base, 0, 0, 0});
    return TRUE;
}

// Decommitted pages fall out of write watching, and an older snapshot needs them back: instead they stay committed and
// are zeroed (what the game reads after committing them again).
bool ZeroRange(void* p, std::size_t n)
{
    __try {
        std::memset(p, 0, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

BOOL WINAPI HoldDecommit(std::uintptr_t address, std::size_t size)
{
    if (!g_active) return FALSE;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi)) return FALSE;
    const std::uintptr_t lo = address & ~(kPage - 1);
    const std::uintptr_t hi = size ? (address + size + kPage - 1) & ~(kPage - 1)
                                   : reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    for (std::uintptr_t p = lo; p < hi;) {
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
        const auto e = std::min<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, hi);
        if (mbi.State == MEM_COMMIT && Writable(mbi.Protect)) ZeroRange(reinterpret_cast<void*>(p), e - p);
        p = e;
    }
    std::lock_guard lock(g_statsMutex);
    ++g_stats.heldDecommits;
    return TRUE;
}

BOOL WINAPI HoldClose(HANDLE handle)
{
    if (!g_active) return FALSE;
    std::lock_guard lock(g_eventMutex);
    g_run.push_back({g_frame, EventKind::Close, reinterpret_cast<std::uintptr_t>(handle), 0, 0, 0});
    return TRUE;
}

// The game's new reservations: logged, and after a restore handed the addresses they got the first time.
LPVOID WINAPI ReplayAlloc(SIZE_T size, DWORD type, DWORD protect)
{
    // Only the game thread's: the workers' come in no particular order (theirs are made the normal way)
    if (!g_active || !engine::IsGameThread()) return nullptr;
    void* result = nullptr;
    std::lock_guard lock(g_eventMutex);
    if (!g_replay.empty()) {
        const Event a = g_replay.front();
        g_replay.pop_front();
        if (a.size == size && a.type == type && a.protect == protect) {
            // As fresh as the first time: nothing committed, then what this call commits (zeros)
            g_tracker->VirtualFree(reinterpret_cast<void*>(a.what), a.size, MEM_DECOMMIT);
            if (!(type & MEM_COMMIT) || g_tracker->VirtualAlloc(reinterpret_cast<void*>(a.what), size, MEM_COMMIT, protect)) {
                result = reinterpret_cast<void*>(a.what);
                std::lock_guard s(g_statsMutex);
                ++g_stats.replayedAllocs;
            }
        } else {
            // This run asks for something else (a worker thread allocated in between, or the inputs changed): from here
            // on its reservations are new ones
            spdlog::debug("TAS snapshots: frame {} allocates differently than before", g_frame);
            g_replay.clear();
        }
    }
    if (!result) {
        result = g_tracker->VirtualAlloc(nullptr, size, type | (g_tracker->writeWatch ? MEM_WRITE_WATCH : 0), protect);
        if (!result) return nullptr;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(result);
    g_run.push_back({g_frame, EventKind::Alloc, base, size, type, protect});
    g_sessionAllocs.insert(base);
    return result;
}

// The run becomes part of the recorded timeline: a new keyframe at `frame` holds its state.
void CommitRun(std::uint32_t frame)
{
    std::lock_guard lock(g_eventMutex);
    const auto cut = std::partition_point(g_recorded.begin(), g_recorded.end(), [](const Event& e) { return e.frame < g_runFrom; });
    g_recorded.erase(cut, g_recorded.end());
    g_recorded.insert(g_recorded.end(), g_run.begin(), g_run.end());
    g_run.clear();
    g_runFrom = frame;
}

// The game is in the recorded timeline's state at keyframe `frame` (a restore or a snap): the run is dropped, and the
// recorded reservations from there on are handed out again as the frames run again.
void SyncTo(std::uint32_t frame)
{
    std::lock_guard lock(g_eventMutex);
    g_run.clear();
    g_runFrom = frame;
    g_replay.clear();
    for (const Event& e : g_recorded)
        if (e.kind == EventKind::Alloc && e.frame >= frame) g_replay.push_back(e);
}

// Every version stored after `frame`, and the keyframes there: that future isn't this timeline's any more.
void DropVersionsAfter(std::uint32_t frame)
{
    for (auto it = g_versionPages.upper_bound(frame); it != g_versionPages.end();) {
        for (std::uintptr_t page : it->second) {
            auto pit = g_pages.find(page);
            if (pit == g_pages.end()) continue;
            auto& versions = pit->second;
            while (!versions.empty() && versions.back().frame > frame) {
                g_store.Drop(versions.back().blob);
                versions.pop_back();
            }
            if (versions.empty()) g_pages.erase(pit);
        }
        it = g_versionPages.erase(it);
    }
    g_keys.erase(g_keys.upper_bound(frame), g_keys.end());
}

// Thins keyframe `k` out (not the first nor the last): versions stored with it that the next keyframe has a newer one of
// are dropped; the others stay (they are what the next keyframe holds there too).
void ThinOut(std::map<std::uint32_t, Keyframe>::iterator it)
{
    const std::uint32_t k = it->first;
    const auto next = std::next(it);
    if (it == g_keys.begin() || next == g_keys.end()) return;
    const std::uint32_t n = next->first;
    auto vp = g_versionPages.find(k);
    if (vp != g_versionPages.end()) {
        std::vector<std::uintptr_t> kept;
        for (std::uintptr_t page : vp->second) {
            auto pit = g_pages.find(page);
            if (pit == g_pages.end()) continue;
            auto& versions = pit->second;
            auto v = std::find_if(versions.begin(), versions.end(), [&](const Version& x) { return x.frame == k; });
            if (v == versions.end()) continue;
            const auto after = std::next(v);
            if (after != versions.end() && after->frame <= n) {
                g_store.Drop(v->blob);
                versions.erase(v);
            } else {
                kept.push_back(page);
            }
        }
        if (kept.empty()) g_versionPages.erase(vp);
        else vp->second = std::move(kept);
    }
    d3dpins::Forget(k);
    g_keys.erase(it);
}

// Over the budget: thin keyframes out, far from where the session is and where they are dense first. The first (the
// level start), the last and the ones within two seconds of the anchor stay.
void ThinToBudget()
{
    if (g_store.Bytes() <= g_budget || g_keys.size() < 3) return;
    const std::uint32_t anchor = g_anchor ? *g_anchor : g_keys.rbegin()->first;
    while (g_store.Bytes() > g_budget) {
        auto best = g_keys.end();
        double bestScore = 0.0;
        for (auto it = std::next(g_keys.begin()); it != g_keys.end() && std::next(it) != g_keys.end(); ++it) {
            const std::uint32_t k = it->first;
            const double distance = k > anchor ? k - anchor : anchor - k;
            if (distance < 120.0) continue;
            const double gap = static_cast<double>(std::next(it)->first - std::prev(it)->first);
            const double score = distance / gap;               // far away and close to its neighbours
            if (score > bestScore) {
                bestScore = score;
                best = it;
            }
        }
        if (best == g_keys.end()) break;
        ThinOut(best);
    }
}

void UpdateStats(std::size_t pages)
{
    std::lock_guard lock(g_statsMutex);
    g_stats.active = g_active;
    g_stats.keyframes = g_keys.size();
    g_stats.storedBytes = g_store.Bytes();
    g_stats.lastPages = pages;
}

// ---- compressing in the background ---------------------------------------------------------------------------------------------
// A keyframe's pages are copied on the game thread (a few ms) and compressed by workers; the versions join g_pages when
// the job is done (Flush: before anything reads them). The base is compressed straight from the game's memory, by every
// worker at once, while the game's threads stand still.
struct Job {
    std::uint32_t frame = 0;
    std::vector<std::uintptr_t> pages;
    std::vector<std::uint8_t> data;          // copies of the pages, or empty: read them where they are (frozen)
    std::vector<Blob> blobs;
    std::atomic<int> parts{0};
    std::atomic<bool> failed{false};
};

// Work for the pool: a compression job's part, or a range of a parallel loop (fn)
struct WorkerContext {
    ZSTD_DCtx* dctx = nullptr;
};
struct Loop {
    std::function<void(std::size_t, WorkerContext&)> fn;
    std::atomic<int> parts{0};
};
struct Task {
    std::shared_ptr<Job> job;
    std::shared_ptr<Loop> loop;
    std::size_t lo = 0, hi = 0;
};

std::mutex g_taskMutex;
std::condition_variable g_taskCv, g_doneCv;
std::deque<Task> g_tasks;
std::vector<std::thread> g_workers;
bool g_workersQuit = false;
std::deque<std::shared_ptr<Job>> g_jobs;      // in order, not joined into g_pages yet (game thread)

void WorkerMain()
{
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    WorkerContext context;
    context.dctx = ZSTD_createDCtx();
    std::vector<std::uint8_t> scratch;
    alignas(16) std::uint8_t page[kPage];
    for (;;) {
        Task t;
        {
            std::unique_lock lock(g_taskMutex);
            g_taskCv.wait(lock, [] { return g_workersQuit || !g_tasks.empty(); });
            if (g_tasks.empty()) break;
            t = std::move(g_tasks.front());
            g_tasks.pop_front();
        }
        if (t.loop) {
            for (std::size_t i = t.lo; i < t.hi; ++i) t.loop->fn(i, context);
            if (--t.loop->parts == 0) {
                std::lock_guard lock(g_taskMutex);
                g_doneCv.notify_all();
            }
            continue;
        }
        Job& job = *t.job;
        for (std::size_t i = t.lo; i < t.hi && !job.failed; ++i) {
            const std::uint8_t* src = job.data.empty() ? reinterpret_cast<const std::uint8_t*>(job.pages[i])
                                                       : job.data.data() + i * kPage;
            if (job.data.empty()) {
                if (!CopyPage(page, src)) {                 // gone: stored as zeros (it isn't the game's any more)
                    job.blobs[i] = {};
                    continue;
                }
                src = page;
            }
            job.blobs[i] = StorePage(cctx, src, scratch);
            if (job.blobs[i].size == 0xFFFFFFFFu) job.failed = true;
        }
        if (--job.parts == 0) {
            std::lock_guard lock(g_taskMutex);
            g_doneCv.notify_all();
        }
    }
    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(context.dctx);
}

// Runs fn(i) for i in [0, n) on the workers and waits for them.
void ParallelFor(std::size_t n, std::function<void(std::size_t, WorkerContext&)> fn)
{
    if (n == 0) return;
    auto loop = std::make_shared<Loop>();
    loop->fn = std::move(fn);
    const std::size_t parts = std::max<std::size_t>(1, std::min<std::size_t>(g_workers.size() * 2, (n + 63) / 64));
    loop->parts = static_cast<int>(parts);
    {
        std::lock_guard lock(g_taskMutex);
        for (std::size_t i = 0; i < parts; ++i) g_tasks.push_back({nullptr, loop, n * i / parts, n * (i + 1) / parts});
    }
    g_taskCv.notify_all();
    std::unique_lock lock(g_taskMutex);
    g_doneCv.wait(lock, [&] { return loop->parts.load() == 0; });
}

void StartWorkers()
{
    if (!g_workers.empty()) return;
    g_workersQuit = false;
    const unsigned n = std::clamp<unsigned>(std::thread::hardware_concurrency() / 2, 1, 4);
    for (unsigned i = 0; i < n; ++i) g_workers.emplace_back(WorkerMain);
}

void StopWorkers()
{
    {
        std::lock_guard lock(g_taskMutex);
        g_workersQuit = true;
        g_tasks.clear();
    }
    g_taskCv.notify_all();
    for (auto& t : g_workers) t.join();
    g_workers.clear();
}

void Submit(const std::shared_ptr<Job>& job)
{
    job->blobs.assign(job->pages.size(), Blob{});
    const std::size_t n = job->pages.size();
    const std::size_t parts = std::max<std::size_t>(1, std::min<std::size_t>(g_workers.size() * 4, (n + 255) / 256));
    job->parts = static_cast<int>(parts);
    {
        std::lock_guard lock(g_taskMutex);
        for (std::size_t i = 0; i < parts; ++i) g_tasks.push_back({job, nullptr, n * i / parts, n * (i + 1) / parts});
    }
    g_taskCv.notify_all();
}

void WaitJob(const Job& job)
{
    std::unique_lock lock(g_taskMutex);
    g_doneCv.wait(lock, [&] { return job.parts.load() == 0; });
}

// Waits for the jobs and joins their versions into g_pages. False if one failed (out of memory): the snapshots can't be
// trusted then.
bool Flush()
{
    bool ok = true;
    while (!g_jobs.empty()) {
        const std::shared_ptr<Job> job = g_jobs.front();
        g_jobs.pop_front();
        WaitJob(*job);
        if (job->failed) {
            for (const Blob& b : job->blobs) g_store.Drop(b);
            ok = false;
            continue;
        }
        for (std::size_t i = 0; i < job->pages.size(); ++i) g_pages[job->pages[i]].push_back({job->frame, job->blobs[i]});
    }
    return ok;
}

// Stores the pages as versions of `frame` (a new keyframe). `readInPlace`: the game's threads stand still until this
// returns, so the workers read the pages where they are (the base); otherwise they are copied first.
bool StorePages(std::uint32_t frame, const std::vector<std::uintptr_t>& pages, bool readInPlace)
{
    auto job = std::make_shared<Job>();
    job->frame = frame;
    job->pages.reserve(pages.size());
    if (!readInPlace) job->data.resize(pages.size() * kPage);
    for (std::uintptr_t p : pages) {
        if (readInPlace) {
            job->pages.push_back(p);
            continue;
        }
        std::uint8_t* dst = job->data.data() + job->pages.size() * kPage;
        if (!CopyPage(dst, reinterpret_cast<const void*>(p))) continue;     // decommitted meanwhile
        job->pages.push_back(p);
        if (IsUnwatched(p)) {
            auto& slot = g_shadow[p];
            if (!slot) slot = std::make_unique<std::uint8_t[]>(kPage);
            std::memcpy(slot.get(), dst, kPage);
        }
    }
    if (!readInPlace) job->data.resize(job->pages.size() * kPage);
    g_keys[frame];
    g_versionPages[frame] = job->pages;
    Submit(job);
    g_jobs.push_back(job);
    if (readInPlace) {
        WaitJob(*job);                                       // before the game's threads move again
        if (!Flush()) {
            SetError("out of memory for snapshots");
            return false;
        }
    } else if (g_jobs.size() > 4 && !Flush()) {               // the workers fall behind: catch up now
        SetError("out of memory for snapshots");
        return false;
    }
    return true;
}

} // namespace

// ---- public ---------------------------------------------------------------------------------------------------------------------
bool Init()
{
    // The loader's tracker: the dinput8.dll in the game's folder (the system one has no such export)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof me;
        for (BOOL ok = Module32FirstW(snap, &me); ok && !g_tracker; ok = Module32NextW(snap, &me))
            if (auto fn = reinterpret_cast<memenu_tracker::GetTracker_t>(GetProcAddress(me.hModule, memenu_tracker::kExport)))
                g_tracker = fn();
        CloseHandle(snap);
    }
    if (!g_tracker) {
        g_why = "the game wasn't started with the menu's loader (dinput8.dll), or its memory tracking is off";
    } else if (g_tracker->version != memenu_tracker::kVersion || g_tracker->size != sizeof(memenu_tracker::Tracker)) {
        g_why = "the loader (dinput8.dll) is from another version of the menu: reinstall";
        g_tracker = nullptr;
    } else if (!g_tracker->writeWatch) {
        g_why = "Windows (or Wine) doesn't support write watching";
        g_tracker = nullptr;
    } else {
        g_why.clear();
        spdlog::info("TAS snapshots: the loader tracks {} reservations of the game{}", g_tracker->Count(),
                     g_tracker->dropped ? " (some were dropped: its table was full)" : "");
    }
    if (!g_tracker) spdlog::warn("TAS snapshots unavailable: {}", g_why);
    return g_tracker != nullptr;
}

bool Available() { return g_tracker != nullptr && d3dpins::Installed(); }

std::string WhyUnavailable()
{
    if (!g_why.empty()) return g_why;
    if (!d3dpins::Installed()) return "the Direct3D hooks aren't installed yet";
    return {};
}

bool Begin(std::uint32_t frame)
{
    End();
    if (!Available()) {
        SetError(WhyUnavailable());
        return false;
    }
    const double t0 = NowMs();
    if (!g_dctx) g_dctx = ZSTD_createDCtx();
    StartWorkers();
    StopRenderingThread();
    FindGameModulesAndHeaps();
    FindGameThreads();
    g_unwatched = Unwatched();

    Frozen frozen(3000.0);
    if (!frozen.ok()) {
        SetError("the game's threads didn't settle: no snapshots");
        return false;
    }
    g_frame = frame;
    {
        std::lock_guard lock(g_eventMutex);
        g_recorded.clear();
        g_run.clear();
        g_replay.clear();
        g_sessionAllocs.clear();
        g_runFrom = frame;
    }
    // The base: every page of the game's
    const auto watched = Watched();
    std::vector<std::uintptr_t> pages;
    for (const Region& r : watched) ForCommittedPages(r.base, r.base + r.size, [&](std::uintptr_t p) { pages.push_back(p); });
    for (const Region& r : g_unwatched)
        for (std::uintptr_t p = r.base; p < r.base + r.size; p += kPage) pages.push_back(p);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    ResetWatch(watched);
    g_active = true;
    if (!StorePages(frame, pages, true)) {
        g_active = false;
        g_keys.clear();
        g_versionPages.clear();
        g_pages.clear();
        g_store.Clear();
        g_shadow.clear();
        return false;
    }
    for (std::uintptr_t p : pages)
        if (IsUnwatched(p) && !g_shadow.count(p)) UpdateShadow(p);
    g_keys[frame].tls = CaptureTls(frozen);
    CaptureStack(g_keys[frame]);
    g_anchor = frame;

    diag::LogGameCrashesUntil(~0ull, &DescribeReleased);   // the whole session (and a minute after it)
    g_tracker->holdFree = &HoldFree;
    g_tracker->holdClose = &HoldClose;
    g_tracker->holdDecommit = &HoldDecommit;
    g_tracker->replayAlloc = &ReplayAlloc;
    d3dpins::Begin();
    d3dpins::SetFrame(frame);
    d3dpins::Capture(frame);

    const double ms = NowMs() - t0;
    {
        std::lock_guard lock(g_statsMutex);
        g_stats = {};
        g_stats.beginMs = ms;
        g_stats.baseBytes = pages.size() * kPage;
    }
    UpdateStats(pages.size());
    spdlog::info("TAS snapshots: session started at frame {}: {} MB of the game's memory ({} reservations, {} MB other "
                 "data), {} MB stored, {:.0f} ms{}",
                 frame, pages.size() * kPage >> 20, watched.size(),
                 [&] { std::size_t n = 0; for (auto& r : g_unwatched) n += r.size; return n >> 20; }(),
                 g_store.Bytes() >> 20, ms, "");
    return true;
}

void End()
{
    if (!g_active.exchange(false)) return;
    Flush();                                 // the workers stay (made once: a thread starting at the start of a frame
    g_jobs.clear();                          // can hold the loader lock), until Shutdown
    g_tracker->holdFree = nullptr;
    g_tracker->holdClose = nullptr;
    g_tracker->holdDecommit = nullptr;
    g_tracker->replayAlloc = nullptr;
    d3dpins::End();
    std::size_t frees = 0, closes = 0, orphans = 0;
    {
        std::lock_guard lock(g_eventMutex);
        // The timeline the session ended in: recorded before g_runFrom, then the run
        std::vector<Event> final;
        for (const Event& e : g_recorded)
            if (e.frame < g_runFrom) final.push_back(e);
        final.insert(final.end(), g_run.begin(), g_run.end());
        std::unordered_set<std::uintptr_t> live, freed;
        std::unordered_set<std::uintptr_t> closed;
        for (const Event& e : final) {
            if (e.kind == EventKind::Alloc) {
                live.insert(e.what);
                freed.erase(e.what);
            } else if (e.kind == EventKind::Free) {
                live.erase(e.what);
                freed.insert(e.what);
            } else {
                closed.insert(e.what);
            }
        }
        // What it released and closed is let go of for real now; the session's reservations it doesn't hold are released
        std::lock_guard rl(g_releasedMutex);
        g_released.clear();
        const auto release = [&](std::uintptr_t base, const char* why) {
            g_released.push_back({base, ReservationSize(base), why});
            return g_tracker->Release(base) ? 1 : 0;
        };
        for (std::uintptr_t base : freed) frees += release(base, "released by the game");
        for (std::uintptr_t base : g_sessionAllocs)
            if (!live.count(base) && !freed.count(base)) orphans += release(base, "made in a timeline that was dropped");
        for (std::uintptr_t handle : closed) closes += g_tracker->CloseGameHandle(reinterpret_cast<HANDLE>(handle)) ? 1 : 0;
        g_recorded.clear();
        g_run.clear();
        g_replay.clear();
        g_sessionAllocs.clear();
    }
    g_keys.clear();
    g_versionPages.clear();
    g_pages.clear();
    g_store.Clear();
    g_shadow.clear();
    g_unwatched.clear();
    g_anchor.reset();
    ForgetThreads();
    diag::LogGameCrashesUntil(GetTickCount64() + 60'000, &DescribeReleased);
    UpdateStats(0);
    spdlog::info("TAS snapshots: session ended ({} reservations and {} handles let go of, {} unused reservations "
                 "released, {} decommits zeroed instead)", frees, closes, orphans, GetStats().heldDecommits);
}

bool Active() { return g_active; }

void SetFrame(std::uint32_t frame)
{
    g_frame = frame;
    d3dpins::SetFrame(frame);
}

bool Capture(std::uint32_t frame)
{
    if (!g_active) return false;
    if (!g_keys.empty() && g_keys.rbegin()->first >= frame) {
        SetError("keyframe " + std::to_string(frame) + " isn't after the last one");
        return false;
    }
    const double t0 = NowMs();
    const auto unwatched = Unwatched();
    const double t1 = NowMs();
    Frozen frozen(50.0);
    if (!frozen.ok()) return false;                          // busy: the next frame then
    const double t2 = NowMs();
    g_unwatched = unwatched;
    const auto watched = Watched();
    const double t3 = NowMs();
    const auto pages = Dirty(watched, true);
    const double t4 = NowMs();
    const bool stored = StorePages(frame, pages, false);
    const double t5 = NowMs();
    spdlog::debug("TAS snapshots: keyframe {}: {} pages; unwatched {:.1f} ms, freeze {:.1f}, list {:.1f}, dirty {:.1f}, "
                 "store {:.1f} ms", frame, pages.size(), t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4);
    if (!stored) {
        // The pages written since the last keyframe are forgotten now: no restore could be trusted
        SetError("keyframe " + std::to_string(frame) + " couldn't be stored: snapshots stopped");
        End();
        return false;
    }
    g_keys[frame].tls = CaptureTls(frozen);
    CaptureStack(g_keys[frame]);
    g_anchor = frame;
    CommitRun(frame);
    d3dpins::Capture(frame);
    if (g_store.Bytes() > g_budget) {
        Flush();
        ThinToBudget();
    }
    const double ms = NowMs() - t0;
    {
        std::lock_guard lock(g_statsMutex);
        g_stats.lastCaptureMs = ms;
    }
    UpdateStats(pages.size());
    return true;
}

bool Restore(std::uint32_t frame)
{
    if (g_active && !Flush()) {
        SetError("snapshots lost (out of memory): stopped");
        End();
        return false;
    }
    if (!g_active || !g_keys.count(frame) || !g_anchor) {
        SetError("no keyframe " + std::to_string(frame));
        return false;
    }
    const double t0 = NowMs();
    Frozen frozen(3000.0);
    if (!frozen.ok()) {
        SetError("the game's threads didn't settle: couldn't go back");
        return false;
    }
    const double tFrozen = NowMs();
    const auto watched = Watched();
    std::vector<std::uintptr_t> pages = Dirty(watched, true);
    const double tDirty = NowMs();
    const std::uint32_t anchor = *g_anchor;
    PagesBetween(std::min(anchor, frame), std::max(anchor, frame), pages);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    // The versions' compressed bytes in one buffer (the store maps its sections one thread at a time), then the workers
    // decompress them straight into the game's pages. Pages the game gave back meanwhile are written here after.
    struct Write {
        std::uintptr_t page;
        Blob blob;
        std::uint32_t offset;
        bool ok;
    };
    std::vector<Write> writes;
    writes.reserve(pages.size());
    static std::vector<std::uint8_t> packed;
    packed.clear();
    std::size_t failed = 0;
    for (std::uintptr_t p : pages) {
        const Version* v = VersionAt(p, frame);
        if (!v) continue;                                    // not the game's memory at that keyframe
        Write w{p, v->blob, static_cast<std::uint32_t>(packed.size()), false};
        if (v->blob.size != 0) {
            const std::uint8_t* data = g_store.Get(v->blob);
            if (!data) {
                ++failed;
                continue;
            }
            packed.insert(packed.end(), data, data + v->blob.size);
        }
        writes.push_back(w);
    }
    ParallelFor(writes.size(), [&](std::size_t i, WorkerContext& ctx) {
        Write& w = writes[i];
        void* dst = reinterpret_cast<void*>(w.page);
        if (w.blob.size == 0) w.ok = ZeroPage(dst);
        else if (w.blob.size == kPage) w.ok = CopyPage(dst, packed.data() + w.offset);
        else w.ok = DecompressInto(ctx.dctx, dst, packed.data() + w.offset, w.blob.size);
    });
    alignas(16) static std::uint8_t buffer[kPage];
    std::size_t written = 0;
    std::vector<std::uintptr_t> touched;
    for (Write& w : writes) {
        if (!w.ok) {
            // Not committed (or not writable) any more: the slow way
            const Version* v = VersionAt(w.page, frame);
            if (!v || !LoadVersion(v->blob, buffer) || !WritePage(w.page, buffer)) {
                ++failed;
                continue;
            }
        }
        ++written;
        touched.push_back(w.page);
        if (IsUnwatched(w.page)) UpdateShadow(w.page);
    }
    const double tWritten = NowMs();
    ResetTouched(watched, touched);                          // our own writes aren't the game's
    const double tReset = NowMs();
    RestoreTls(g_keys[frame].tls, frozen);
    if (!RestoreStack(g_keys[frame])) spdlog::warn("TAS snapshots: keyframe {}: the engine's stack isn't where it was", frame);
    SyncTo(frame);
    d3dpins::Restore(frame);
    const double tD3d = NowMs();
    g_anchor = frame;
    g_frame = frame;
    const double ms = NowMs() - t0;
    {
        std::lock_guard lock(g_statsMutex);
        g_stats.lastRestoreMs = ms;
        ++g_stats.restores;
    }
    UpdateStats(written);
    spdlog::debug("TAS snapshots: keyframe {} put back (from {}): {} pages in {:.1f} ms (freeze {:.1f}, dirty {:.1f}, "
                 "write {:.1f}, reset {:.1f}, d3d {:.1f})", frame, anchor, written, ms, tFrozen - t0, tDirty - tFrozen,
                 tWritten - tDirty, tReset - tWritten, tD3d - tReset);
    if (failed) SetError(std::to_string(failed) + " pages couldn't be put back at keyframe " + std::to_string(frame));
    for (auto fn : g_onRestore) fn();
    return failed == 0;
}

std::optional<std::uint32_t> Nearest(std::uint32_t frame)
{
    auto it = g_keys.upper_bound(frame);
    if (it == g_keys.begin()) return std::nullopt;
    return std::prev(it)->first;
}

bool Has(std::uint32_t frame) { return g_keys.count(frame) != 0; }

void DropAfter(std::uint32_t frame)
{
    if (!g_active) return;
    Flush();
    // The base stays whatever happens
    DropVersionsAfter(std::max(frame, g_keys.empty() ? frame : g_keys.begin()->first));
    d3dpins::DropAfter(frame);
    UpdateStats(0);
}

std::optional<std::uint32_t> Anchor() { return g_anchor; }

std::size_t Verify(std::uint32_t frame, bool snap)
{
    if (!g_active || !g_keys.count(frame) || !g_anchor || *g_anchor >= frame) return 0;
    if (!Flush()) {
        SetError("snapshots lost (out of memory): stopped");
        End();
        return 0;
    }
    Frozen frozen(3000.0);
    if (!frozen.ok()) return 0;
    const auto watched = Watched();
    std::vector<std::uintptr_t> pages = Dirty(watched, false);   // reset only once the state is the keyframe's
    PagesBetween(*g_anchor, frame, pages);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    alignas(16) static std::uint8_t want[kPage];
    std::size_t differ = 0, checked = 0;
    std::uintptr_t firstDiff = 0;
    for (std::uintptr_t p : pages) {
        const Version* v = VersionAt(p, frame);
        if (!v || !LoadVersion(v->blob, want)) continue;
        ++checked;
        bool same = false;
        if (!SamePage(reinterpret_cast<const void*>(p), want, same) || !same) {
            if (!differ) firstDiff = p;
            ++differ;
            if (snap) WritePage(p, want);
        }
    }
    if (snap) {
        RestoreTls(g_keys[frame].tls, frozen);
        RestoreStack(g_keys[frame]);
    }
    if (snap || differ == 0) {
        SyncTo(frame);
        if (differ) d3dpins::Restore(frame);
        // The state is the keyframe's now: anchor there
        for (std::uintptr_t p : pages)
            if (IsUnwatched(p)) UpdateShadow(p);
        ResetWatch(watched);
        g_anchor = frame;
    }   // else: left as it is, still anchored where it was (write watch and shadow untouched)
    {
        std::lock_guard lock(g_statsMutex);
        g_stats.verifiedPages += checked;
        g_stats.mismatchedPages += differ;
    }
    if (differ)
        spdlog::warn("TAS snapshots: keyframe {} reached again with {} of {} pages different (first at {:#x}){}", frame,
                     differ, checked, firstDiff, snap ? ": put back as recorded" : "");
    return differ;
}

Stats GetStats()
{
    std::lock_guard lock(g_statsMutex);
    Stats s = g_stats;
    s.active = g_active;
    return s;
}

void OnRestore(void (*callback)()) { g_onRestore.push_back(callback); }

bool RenderingThreadOff() { return g_renderingThreadOff; }

void Shutdown()
{
    End();
    StopWorkers();
}

void SetBudget(std::uint64_t bytes)
{
    g_budget = std::max<std::uint64_t>(bytes, 256ull << 20);
    if (g_active && g_store.Bytes() > g_budget) {
        Flush();
        ThinToBudget();
        UpdateStats(0);
    }
}

} // namespace tas::snapshot
