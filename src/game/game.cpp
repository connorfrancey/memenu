#include "game.h"
#include "game/uobject.h"
#include "core/cache.h"
#include "unload.h"

#include <Windows.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <ME_SDK.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
    std::atomic<bool> g_ready{false};
    std::atomic<bool> g_loading{false};
    std::atomic<bool> g_trackCamera{false};
    std::mutex g_cameraMutex;
    rmath::Camera g_camera;
    std::atomic<bool> g_blockInput{false};      // Menu open: ignore look input
    std::atomic<bool> g_inputSuspended{false};  // A TAS session owns the input
    std::atomic<bool> g_inputApplied{false};    // We currently own the controller's bIgnoreLookInput
    bool g_prevIgnoreLook = false;
    std::atomic<unsigned long long> g_ticks{0};
    std::atomic<bool> g_hasPawn{false};
    std::atomic<int> g_health{0}, g_maxHealth{0};
    std::atomic<void*> g_playerPawn{nullptr};   // For hooks that run for every pawn: is this the player?

    std::mutex g_mutex;                         // Guards the two vectors below
    std::vector<game::TickCallback> g_tickCallbacks;
    std::vector<std::function<void()>> g_queue;
    std::vector<game::LevelLoadCallback> g_levelLoadCallbacks;
    std::vector<game::StateChangeCallback> g_stateChangeCallbacks;   // Run by the state handler hook

    // The local player's world and controller, game thread only. Kept between ticks while their slots in the object
    // list still hold them (checked once per tick, without touching the objects: see ValidateCache), dropped when a
    // level starts loading. When there is none (main menu screens, loading), the object list is scanned again at most
    // every kRescanTicks ticks: a scan walks ~100k objects.
    constexpr unsigned long long kRescanTicks = 15;
    Classes::AWorldInfo* g_world = nullptr;
    Classes::ATdPlayerController* g_controller = nullptr;
    std::size_t g_worldIndex = 0, g_controllerIndex = 0;
    unsigned long long g_validatedTick = ~0ull;     // Tick the cache was last checked in
    unsigned long long g_nextScanTick = 0;          // Nothing found: don't scan again before this tick
    std::atomic<unsigned long long> g_scans{0};

    void DropCache()
    {
        g_world = nullptr;
        g_controller = nullptr;
        g_nextScanTick = 0;
    }

    // Still the same player? Both objects alive, the controller still in the world's list and still set up. A respawn
    // or a restart that replaces them without the level-load hook makes this fail, and the next lookup scans again.
    void ValidateCache()
    {
        g_validatedTick = g_ticks;
        if (!g_controller) return;
        if (!game::AliveAt(g_world, g_worldIndex) || !game::AliveAt(g_controller, g_controllerIndex)) { DropCache(); return; }
        bool listed = false;
        int n = 0;
        for (auto c = g_world->ControllerList; c && n < 64 && !listed; c = c->NextController, ++n) listed = c == g_controller;
        if (!listed || !g_controller->PlayerCamera) DropCache();
    }

    void (__thiscall *oTick)(float*, int, float) = nullptr;
    int (__thiscall *oLevelLoad)(void*, void*, unsigned long long) = nullptr;

    // ---- Pattern scan over the main executable's image --------------------------------------------------------------
    // Pattern/mask as used by mmultiplayer: 'x' = byte must match, '?' = wildcard.
    // Within [from, to) of the image (offsets from its base), clamped to the image.
    void* FindPatternRange(std::size_t from, std::size_t to, const char* pattern, const char* mask)
    {
        const auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const std::size_t imageSize = std::min<std::size_t>(nt->OptionalHeader.SizeOfImage, to);
        const std::size_t len = std::strlen(mask);

        for (std::size_t off = from; off + len <= imageSize;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(base + off, &mbi, sizeof mbi)) break;

            const auto regionStart = static_cast<std::size_t>(static_cast<std::uint8_t*>(mbi.BaseAddress) - base);
            const std::size_t regionEnd = regionStart + mbi.RegionSize;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

            if (readable) {
                const std::size_t end = std::min(regionEnd, imageSize);
                for (std::size_t i = off; i + len <= end; ++i) {
                    bool match = true;
                    for (std::size_t j = 0; j < len && match; ++j)
                        match = mask[j] != 'x' || base[i + j] == static_cast<std::uint8_t>(pattern[j]);
                    if (match) return base + i;
                }
            }
            off = regionEnd;
        }
        return nullptr;
    }

    void* FindPattern(const char* pattern, const char* mask)
    {
        return FindPatternRange(0, ~std::size_t{0}, pattern, mask);
    }

    // ---- Script events (UObject::ProcessEvent) ----------------------------------------------------------------------
    // ProcessEvent runs thousands of times a frame, so the hook only compares the called function against a few cached
    // pointers. The watch list only grows: an entry is written in full before the count that covers it is published.
    constexpr int kMaxWatches = 64;
    enum class WatchKind { Before, After, Block };
    struct Watch
    {
        std::string name;
        WatchKind kind = WatchKind::Before;
        game::ScriptEventCallback callback = nullptr;
        game::ScriptEventFilter filter = nullptr;
    };
    Watch g_watches[kMaxWatches];
    std::atomic<Classes::UFunction*> g_watchFn[kMaxWatches];
    std::atomic<int> g_watchCount{0};
    std::atomic<bool> g_watchLookup{false};     // Some function is still to be looked up (game thread, next tick)
    std::atomic<unsigned long long> g_scriptEvents{0};
    bool g_processEventHooked = false;          // Under g_mutex
    int (__thiscall *oProcessEvent)(Classes::UObject*, Classes::UFunction*, void*, void*) = nullptr;

    // A watch's callback, kept from throwing into the engine. Returns the filter's answer (false for the others).
    bool RunWatch(const Watch& w, Classes::UObject* self, void* params)
    {
        try {
            if (w.kind == WatchKind::Block) return w.filter(self, params);
            w.callback(self, params);
        } catch (const std::exception& e) {
            spdlog::error("Script event {}: {}", w.name, e.what());
        } catch (...) {
            spdlog::error("Script event {}: unknown exception", w.name);
        }
        return false;
    }

    int __fastcall ProcessEventHook(Classes::UObject* self, void* /*edx*/, Classes::UFunction* fn, void* params, void* result)
    {
        const int n = g_watchCount.load(std::memory_order_acquire);
        bool watched = false, skip = false, hasAfter = false;
        for (int i = 0; i < n; ++i) {
            if (!fn || fn != g_watchFn[i].load(std::memory_order_relaxed)) continue;
            watched = true;
            const Watch& w = g_watches[i];
            if (w.kind == WatchKind::After) { hasAfter = true; continue; }
            if (RunWatch(w, self, params)) skip = true;
        }
        if (!watched) return oProcessEvent(self, fn, params, result);
        g_scriptEvents.fetch_add(1, std::memory_order_relaxed);
        if (skip) return 0;     // A filter took the call: the engine never sees it

        const int ret = oProcessEvent(self, fn, params, result);
        if (hasAfter)
            for (int i = 0; i < n; ++i)
                if (fn == g_watchFn[i].load(std::memory_order_relaxed) && g_watches[i].kind == WatchKind::After)
                    RunWatch(g_watches[i], self, params);
        return ret;
    }

    // Game thread. A full object walk per function (SDK FindObject compares full names): only done when something new
    // is watched, and after a level load for the ones that weren't found.
    void LookUpWatches()
    {
        if (!g_watchLookup.exchange(false)) return;
        const int n = g_watchCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) {
            if (g_watchFn[i].load()) continue;
            // Another watch on the same function already found it: no second object walk
            Classes::UFunction* fn = nullptr;
            for (int j = 0; j < n && !fn; ++j)
                if (j != i && g_watches[j].name == g_watches[i].name) fn = g_watchFn[j].load();
            if (fn) { g_watchFn[i].store(fn); continue; }
            fn = Classes::UObject::FindObject<Classes::UFunction>(g_watches[i].name);
            g_watchFn[i].store(fn);
            if (fn) spdlog::info("Script event: watching {}", g_watches[i].name);
            else    spdlog::warn("Script event: {} not found (looked again after the next level load)", g_watches[i].name);
        }
    }

    // Under g_mutex.
    bool HookProcessEvent()
    {
        if (g_processEventHooked) return true;
        // mmultiplayer's pattern for UObject::ProcessEvent; the SDK calls it as virtual function 61, the fallback.
        void* target = FindPattern("\x56\x8B\xF1\x8B\x0D\x00\x00\x00\x00\x85\xC9\x74\x09", "xxxxx????xxxx");
        void* fromVtable = nullptr;
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num() && !fromVtable; ++i)
            if (const auto* o = objects.GetByIndex(i)) fromVtable = (*reinterpret_cast<void* const* const*>(o))[61];
        if (target && fromVtable && target != fromVtable)
            spdlog::warn("ProcessEvent: the pattern ({}) and the vtable ({}) disagree, using the pattern", target, fromVtable);
        if (!target) target = fromVtable;
        if (!target) { spdlog::error("ProcessEvent not found: script events (speedrun auto splits) won't work"); return false; }
        if (MH_CreateHook(target, reinterpret_cast<void*>(&ProcessEventHook), reinterpret_cast<void**>(&oProcessEvent)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            spdlog::error("Could not hook ProcessEvent: script events (speedrun auto splits) won't work");
            return false;
        }
        g_processEventHooked = true;
        spdlog::info("ProcessEvent hooked");
        return true;
    }

    // ---- Hooks ------------------------------------------------------------------------------------------------------
    void DrainQueue()
    {
        std::vector<std::function<void()>> work;
        {
            std::lock_guard lock(g_mutex);
            work.swap(g_queue);
        }
        for (auto& fn : work) fn();
    }

    // Game thread. Sets the controller's bIgnoreLookInput while the menu is open and puts the old value back after.
    void ApplyInputBlock()
    {
        const bool want = g_blockInput && !g_inputSuspended;
        if (!want && !g_inputApplied) return;   // Nothing to do, don't even look the controller up

        auto* pc = game::Controller();
        if (!pc) return;    // Retry next tick

        if (want)
        {
            if (!g_inputApplied) { g_prevIgnoreLook = pc->bIgnoreLookInput != 0; g_inputApplied = true; }
            pc->bIgnoreLookInput = 1;   // Re-applied every tick: a level change replaces the controller
        } else
        {
            pc->bIgnoreLookInput = g_prevIgnoreLook ? 1 : 0;
            g_inputApplied = false;
        }
    }

    // Game thread: copy the final camera (position, rotation, FOV) the world is rendered from.
    void UpdateCamera() {
        rmath::Camera cam;
        if (auto* pc = game::Controller(); pc && pc->PlayerCamera) {
            const auto& pov = pc->PlayerCamera->CameraCache.POV;
            cam.pos = {pov.Location.X, pov.Location.Y, pov.Location.Z};
            cam.pitch = static_cast<int>(pov.Rotation.Pitch);
            cam.yaw = static_cast<int>(pov.Rotation.Yaw);
            cam.roll = static_cast<int>(pov.Rotation.Roll);
            cam.fov = pov.FOV;
            cam.valid = std::isfinite(cam.pos.x) && std::isfinite(cam.pos.y) && std::isfinite(cam.pos.z) &&
                        std::isfinite(cam.fov) && cam.fov > 1.f && cam.fov < 179.f;
        }
        std::lock_guard lock(g_cameraMutex);
        g_camera = cam;
    }

    void UpdateStatus()
    {
        const auto pawn = game::Pawn();
        if ((pawn != nullptr) != g_hasPawn.load())
            spdlog::info(pawn ? "Player pawn found" : "Player pawn lost");
        g_hasPawn = pawn != nullptr;
        if (pawn)
        {
            g_health = pawn->Health;
            g_maxHealth = pawn->MaxHealth;
        }
    }

    void __fastcall TickHook(float* scales, void* /*edx*/, int arg, float delta)
    {
        ++g_ticks;
        if (!g_loading)
        {
            // A respawn or level restart can replace the world/controller without going through the level-load
            // hook, and a stale pointer would be a crash: check the cached ones before anything uses them.
            ValidateCache();

            DrainQueue();
            ApplyInputBlock();
            LookUpWatches();

            std::vector<game::TickCallback> callbacks;
            {
                std::lock_guard lock(g_mutex);
                callbacks = g_tickCallbacks;
            }
            for (auto cb : callbacks) cb(delta);

            g_playerPawn.store(game::Pawn(), std::memory_order_relaxed);
            if (g_ticks % 15 == 0) UpdateStatus();
        }
        oTick(scales, arg, delta);

        // The camera is read *after* the engine has ticked: the player controller updates its camera during the tick, so a
        // read before it is one frame old and a volume drawn from it trails the world as you move (jitter).
        if (!g_loading && g_trackCamera) {
            ValidateCache();            // The tick may have replaced them
            UpdateCamera();
        }
    }

    // There is deliberately no "projection" hook. mmultiplayer hooks the function at "\x83\xEC\x3C\xD9\x44\x24\x44" as
    // `int*(__thiscall*)(FMatrix*, void*)`, but in this game version that function is a visibility test: it reads ecx
    // AND edx as inputs, takes two stack arguments the caller pops (it ends in a plain `ret`) and returns 0/1. Hooked
    // with that signature it unbalanced the stack on every call (the debug build's /RTC check fired on it, release
    // builds crashed later) and passed it a garbage edx. Nothing there is a projection matrix either, so the debug
    // renderer uses its own near plane setting (game::EngineNear() returns 0).

    int __fastcall LevelLoadHook(void* self, void* /*edx*/, void** levelInfo, unsigned long long arg)
    {
        g_loading = true;
        g_playerPawn = nullptr;
        DropCache();
        cache::NextLevel();

        std::vector<game::LevelLoadCallback> callbacks;
        {
            std::lock_guard lock(g_mutex);
            callbacks = g_levelLoadCallbacks;
        }
        for (auto cb : callbacks) cb(false);
        const int ret = oLevelLoad(self, levelInfo, arg);
        g_loading = false;
        for (int i = 0, n = g_watchCount.load(); i < n; ++i)
            if (!g_watchFn[i].load()) g_watchLookup = true;     // Its package may have come with the level
        for (auto cb : callbacks) cb(true);
        return ret;
    }

    // ---- The player's move state handler (mmultiplayer trainer's "StateHandler") ---------------------------------------
    // A pawn's per-tick movement state function: (delta, changedState), changedState non-zero on the tick a new state
    // begins. It runs for every pawn, so the hook only compares the pawn against the player's, kept by the tick hook.
    std::atomic<bool> g_stateHooked{false}, g_deathHooked{false};
    std::atomic<unsigned> g_deaths{0};
    std::vector<game::DeathCallback> g_deathCallbacks;      // Under g_mutex
    void (__thiscall *oStateHandler)(void*, float, int) = nullptr;
    int (*oPreDeath)() = nullptr;
    int (*oPostDeath)() = nullptr;

    void __fastcall StateHandlerHook(void* pawn, void* /*edx*/, float delta, int changedState)
    {
        if (changedState && pawn && pawn == g_playerPawn.load(std::memory_order_relaxed)) {
            std::vector<game::StateChangeCallback> callbacks;
            {
                std::lock_guard lock(g_mutex);
                callbacks = g_stateChangeCallbacks;
            }
            bool skip = false;
            for (auto cb : callbacks) {
                try { skip |= cb(); }
                catch (...) { spdlog::error("Player state change callback threw"); }
            }
            if (skip) return;
        }
        oStateHandler(pawn, delta, changedState);
    }

    void RunDeathCallbacks(bool after)
    {
        std::vector<game::DeathCallback> callbacks;
        {
            std::lock_guard lock(g_mutex);
            callbacks = g_deathCallbacks;
        }
        for (auto cb : callbacks) {
            try { cb(after); }
            catch (...) { spdlog::error("Death callback threw"); }
        }
    }

    int PreDeathHook()
    {
        ++g_deaths;
        spdlog::info("Player died");
        RunDeathCallbacks(false);
        return oPreDeath();
    }

    int PostDeathHook()
    {
        const int ret = oPostDeath();
        RunDeathCallbacks(true);
        return ret;
    }

    bool HookAt(void* target, void* detour, void** original, const char* what);

    // Optional hooks: without them the menu works, only the features built on them don't (they say so in the log).
    void HookExtras()
    {
        // mmultiplayer trainer's pattern. `movzx edx, byte [ecx+68h]` in it reads Actor.Physics: ecx is the pawn.
        if (void* p = FindPattern("\xF3\x0F\x10\x05\x00\x00\x00\x00\x83\xEC\x0C\x0F\x2F\x44\x24\x00\x56"
                                  "\x0F\x87\x00\x00\x00\x00\x8B\x44\x24\x18\x83\xF8\x07\x0F\x8F\x00\x00"
                                  "\x00\x00\x0F\xB6\x51\x68\x81\xA1\x00\x00\x00\x00\x00\x00\x00\x00",
                                  "xxxx????xxxxxxx?xxx????xxxxxxxxx????xxxxxx????????")) {
            g_stateHooked = HookAt(p, reinterpret_cast<void*>(&StateHandlerHook), reinterpret_cast<void**>(&oStateHandler),
                                   "the player state handler");
        } else {
            spdlog::warn("Player state handler not found: Strang will not work");
        }

        // mmultiplayer's death hooks: an anchor, then the two small functions within 4 KB after it
        void* anchor = FindPattern("\x8D\x4C\x24\x10\xE8\x00\x00\x00\x00\x8B\x4C\x24\x14\x85\xC9\x7C"
                                   "\x1E\x3B\xCF\x0F\x8D\x00\x00\x00\x00\x8B\x04\x8E\x8B\x40\x08\x25"
                                   "\x00\x00\x00\x00\x33\xD2\x0B\xC2\x75\xD6\xE9\x00\x00\x00\x00",
                                   "xxxxx????xxxxxxxxxxxx????xxxxxxx????xxxxxxx????");
        void* pre = anchor ? game::FindPatternIn(anchor, 0x1000,
                                                 "\xC7\x05\x00\x00\x00\x00\x00\x00\x00\x00\xB8\x00\x00\x00\x00\xC3"
                                                 "\xB8\x00\x00\x00\x00\xC3",
                                                 "xx????????x????xx????x")
                           : nullptr;
        void* post = pre ? game::FindPatternIn(pre, 0x1000,
                                               "\x8B\x0D\x00\x00\x00\x00\xC7\x05\x00\x00\x00\x00\x00\x00\x00\x00"
                                               "\x8B\x01\x8B\x90\x00\x00\x00\x00\xFF\xD2\xB8\x00\x00\x00\x00\xC3"
                                               "\x8B\xC1\xC7\x00\x00\x00\x00\x00\xC3",
                                               "??????xx????????xxxx????xxx????xxxxx????x")
                         : nullptr;
        if (pre && post) {
            g_deathHooked = HookAt(pre, reinterpret_cast<void*>(&PreDeathHook), reinterpret_cast<void**>(&oPreDeath),
                                   "PreDeath") &&
                            HookAt(post, reinterpret_cast<void*>(&PostDeathHook), reinterpret_cast<void**>(&oPostDeath),
                                   "PostDeath");
        } else {
            spdlog::warn("Death functions not found (anchor {}, pre {}, post {}): no death events", anchor, pre, post);
        }
        spdlog::info("Extra hooks: player state handler {}, death {}", g_stateHooked ? "on" : "off",
                     g_deathHooked ? "on" : "off");
    }

    bool HookAt(void* target, void* detour, void** original, const char* what)
    {
        if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
            spdlog::error("Could not hook {}", what);
            return false;
        }
        return true;
    }
}

// ---- Early injection ------------------------------------------------------------------------------------------------
// Every SDK StaticClass() and function wrapper looks its object up once and keeps the answer in a static, a null one too.
// A loader that injects the menu before the game has loaded its script packages (TinyModInjector on Linux/Proton does:
// 1.6k names instead of ~43k) would make those lookups find nothing for the whole session: no player, no IsA, no SDK
// calls. So game::Init waits for TdGame's classes before anything asks for one.
namespace {
    // Index of `name` in the engine's name table, -1 if it isn't there (yet). No allocation, no lock: it runs inside the
    // guarded walk below.
    int RawNameIndex(const wchar_t* name)
    {
        const auto& names = Classes::FName::GetGlobalNames();
        for (std::size_t i = 0; i < names.Num(); ++i) {
            const Classes::FNameEntry* e = names[i];
            if (e && std::wcscmp(e->GetWideName(), name) == 0) return static_cast<int>(i);
        }
        return -1;
    }

    // A UClass named TdPlayerPawn (an object of that name whose own class is "Class"). Reads the game's name and object
    // lists while the game is still loading them (they can grow under us).
    bool HasPawnClass()
    {
        const int pawn = RawNameIndex(L"TdPlayerPawn"), cls = RawNameIndex(L"Class");
        if (pawn < 0 || cls < 0) return false;
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num(); ++i) {
            const Classes::UObject* o = objects.GetByIndex(i);
            if (o && o->Name.Index == pawn && o->Class && o->Class->Name.Index == cls) return true;
        }
        return false;
    }

    // The MSVC build guards the walk: a list reallocated mid-read is a miss, tried again 100 ms later.
    bool ScriptClassesLoaded()
    {
#ifdef _MSC_VER
        __try { return HasPawnClass(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
        return HasPawnClass();
#endif
    }

    // Returns false if the menu is asked to unload meanwhile, or the classes never turn up.
    bool WaitForScriptClasses()
    {
        if (ScriptClassesLoaded()) return true;
        spdlog::info("Game: the menu was loaded before the game's script classes ({} names so far): waiting for them",
                     Classes::FName::GetGlobalNames().Num());
        constexpr ULONGLONG kTimeoutMs = 180000;
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < kTimeoutMs) {
            if (unload::Requested()) {
                SetEvent(unload::Event());     // Requested() took the END press: the menu's main loop must still see it
                return false;
            }
            Sleep(100);
            if (ScriptClassesLoaded()) {
                Sleep(500);     // Its package is still being linked: let the rest of its classes come in
                spdlog::info("Game: script classes loaded after {} ms ({} names, {} objects)", GetTickCount64() - start,
                             Classes::FName::GetGlobalNames().Num(), Classes::UObject::GetGlobalObjects().Num());
                return true;
            }
        }
        spdlog::error("Game: TdGame's classes did not load within {} s: game features will not work",
                      kTimeoutMs / 1000);
        return false;
    }
}

bool game::CheckObjectLayout()
{
    // A class must reach Actor and Object through SuperOf(), and Actor's own fields must include Location.
    const auto* pawnClass = Classes::ATdPlayerPawn::StaticClass();
    const auto* actorClass = Classes::AActor::StaticClass();
    if (!pawnClass || !actorClass) {
        spdlog::error("Object layout: TdPlayerPawn/Actor classes not found");
        return false;
    }
    bool reachesActor = false;
    int depth = 0;
    for (const Classes::UStruct* c = pawnClass; c && depth < 64; c = SuperOf(c), ++depth)
        if (c == actorClass) { reachesActor = true; break; }
    int fields = 0;
    bool hasLocation = false;
    for (Classes::UField* f = ChildrenOf(actorClass); f && fields < 10000; f = NextOf(f), ++fields)
        if (f->GetName() == "Location") hasLocation = true;
    if (!reachesActor || !hasLocation) {
        spdlog::error("Object layout: does not match this game (TdPlayerPawn reaches Actor: {}, Actor has {} fields, "
                      "Location among them: {}): class reflection (save state, IsA) will not work",
                      reachesActor, fields, hasLocation);
        return false;
    }
    spdlog::info("Object layout: ok (TdPlayerPawn -> Actor in {} steps, Actor declares {} fields)", depth, fields);
    return true;
}

bool game::Init()
{
    void* ptr = nullptr;

    // These byte patterns identify the game's own code, so they only match the game version they were made for
    // (copied verbatim from mmultiplayer's engine.cpp).
    ptr = FindPattern(
        "\x8B\x0D\x00\x00\x00\x00\x8B\x84\x24\x00"
        "\x00\x00\x00\x8B\x04\x81",
        "xx????xxx????xxx");
    if (!ptr) { spdlog::error("GNames pattern not found: unsupported game version?"); return false; }
    Classes::FName::GNames = reinterpret_cast<decltype(Classes::FName::GNames)>(
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(ptr) + 2));

    ptr = FindPattern(
        "\x8B\x15\x00\x00\x00\x00\x8B\x0C\xB2\x8D\x44\x24\x30",
        "xx????xxxxxxx");
    if (!ptr) { spdlog::error("GObjects pattern not found: unsupported game version?"); return false; }
    Classes::UObject::GObjects = reinterpret_cast<decltype(Classes::UObject::GObjects)>(
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(ptr) + 2));

    spdlog::info("SDK globals: GNames={} entries, GObjects={} objects",
                 Classes::FName::GetGlobalNames().Num(), Classes::UObject::GetGlobalObjects().Num());
    // Before the first StaticClass() (CheckObjectLayout is one): see WaitForScriptClasses
    if (!WaitForScriptClasses()) return false;
    CheckObjectLayout();   // Only logs: the menu and most features work without class reflection

    ptr = FindPattern(
        "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x81\xEC"
        "\x00\x00\x00\x00\x53\x55\x56\x57\xA1\x00\x00\x00\x00\x33\xC4\x50"
        "\x8D\x84\x24\x00\x00\x00\x00\x64\xA3\x00\x00\x00\x00\x8B\xE9\x89"
        "\x6C\x24\x00\x00\xFF\x89",
        "???????xxxxxxxxx?xxxxxxxx????xxxxxx?xxxxxxxxxxxxxx??xx");
    if (!ptr) { spdlog::error("LevelLoad pattern not found"); return false; }
    if (!HookAt(ptr, reinterpret_cast<void*>(&LevelLoadHook), reinterpret_cast<void**>(&oLevelLoad), "LevelLoad")) return false;

    ptr = FindPattern(
        "\x83\xEC\x48\x53\x55\x56\x57\x8B\xF9\xE8\x00\x00\x00\x00\x8B\x0D"
        "\x00\x00\x00\x00\x8B\x15\x00\x00\x00\x00\x8B\xE8",
        "xxxxxxxxxx????xx????xx????xx");
    if (!ptr) { spdlog::error("Tick pattern not found"); return false; }
    if (!HookAt(ptr, reinterpret_cast<void*>(&TickHook), reinterpret_cast<void**>(&oTick), "Tick")) return false;

    HookExtras();
    g_ready = true;
    {
        // Hooked now, while the menu loads: finding ProcessEvent scans the game's code (~150 ms in a debug build), which
        // the first module to watch a script event otherwise paid in the middle of play. The TAS and the speedrun timer
        // watch events from startup anyway.
        std::lock_guard lock(g_mutex);
        HookProcessEvent();
    }
    spdlog::info("Game integration ready");
    return true;
}

void game::Shutdown()
{
    // The queue is drained by the game's tick; give it a moment so modules can undo their changes.
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard lock(g_mutex);
            if (g_queue.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    g_blockInput = false;       // Let the tick hook hand the controller its look input back
    for (int i = 0; i < 40 && g_inputApplied; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    g_ready = false;
}

void game::SetInputBlocked(bool blocked)
{
    g_blockInput = blocked;
}

void game::SetInputBlockedSuspended(bool suspended)
{
    g_inputSuspended = suspended;
}

void game::OnLevelLoad(LevelLoadCallback callback)
{
    std::lock_guard lock(g_mutex);
    g_levelLoadCallbacks.push_back(callback);
}

void game::OnPlayerStateChange(StateChangeCallback callback)
{
    std::lock_guard lock(g_mutex);      // Kept even before Init: the hook is installed there (HookExtras)
    g_stateChangeCallbacks.push_back(callback);
}

bool game::PlayerStateHooked() { return g_stateHooked; }

void game::OnPlayerDeath(DeathCallback callback)
{
    std::lock_guard lock(g_mutex);
    g_deathCallbacks.push_back(callback);
}

bool game::DeathHooked() { return g_deathHooked; }

bool game::ConsoleCommand(const std::string& command)
{
    auto* pc = Controller();
    if (!pc || command.empty()) return false;
    const std::wstring wide(command.begin(), command.end());    // Console commands are ASCII
    // The FString only borrows the buffer; the engine copies what it keeps. The returned string is the engine's (and
    // usually empty): left alone, freeing it would need the engine's allocator.
    pc->ConsoleCommand(Classes::FString(wide.c_str()), false);
    spdlog::info("Console: {}", command);
    return true;
}

void* game::FindPatternIn(const void* start, std::size_t size, const char* pattern, const char* mask)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto from = reinterpret_cast<std::uintptr_t>(start);
    if (from < base) return nullptr;
    return FindPatternRange(from - base, from - base + size, pattern, mask);
}

namespace {
    bool AddWatch(const char* fullName, Watch w)
    {
        if (!g_ready || !fullName) return false;
        std::lock_guard lock(g_mutex);
        if (!HookProcessEvent()) return false;
        const int n = g_watchCount.load();
        if (n >= kMaxWatches) { spdlog::error("Script event {}: too many watched already", fullName); return false; }
        w.name = fullName;
        g_watches[n] = std::move(w);
        g_watchFn[n].store(nullptr);
        g_watchCount.store(n + 1, std::memory_order_release);   // Published last: the hook only reads complete entries
        g_watchLookup = true;
        return true;
    }
}

bool game::OnScriptEvent(const char* fullName, ScriptEventCallback callback, bool after)
{
    if (!callback) return false;
    Watch w;
    w.kind = after ? WatchKind::After : WatchKind::Before;
    w.callback = callback;
    return AddWatch(fullName, std::move(w));
}

bool game::BlockScriptEvent(const char* fullName, ScriptEventFilter filter)
{
    if (!filter) return false;
    Watch w;
    w.kind = WatchKind::Block;
    w.filter = filter;
    return AddWatch(fullName, std::move(w));
}

bool game::ScriptEventsReady()
{
    {
        std::lock_guard lock(g_mutex);
        if (!g_processEventHooked) return false;
    }
    const int n = g_watchCount.load();
    for (int i = 0; i < n; ++i)
        if (!g_watchFn[i].load()) return false;
    return n > 0;
}

bool game::BlockingLoad()
{
    const auto* world = World();
    return world && world->bRequestedBlockOnAsyncLoading;
}

std::string game::MapName()
{
    Classes::UObject* o = World();      // The persistent level's package: follow Outer up from the WorldInfo
    if (!o) return {};
    for (int guard = 0; o->Outer && guard < 16; ++guard) o = o->Outer;
    return o->GetName();
}

void* game::FindPattern(const char* pattern, const char* mask)
{
    return ::FindPattern(pattern, mask);
}

void game::SetCameraTracking(bool enabled) {
    g_trackCamera = enabled;
}

float game::EngineNear() {
    return 0.f;     // Unknown: no source for the engine's projection yet (see the note above LevelLoadHook)
}

rmath::Camera game::GetCamera() {
    std::lock_guard lock(g_cameraMutex);
    return g_camera;
}

game::Status game::GetStatus()
{
    Status s;
    s.ready = g_ready;
    s.loading = g_loading;
    s.hasPawn = g_hasPawn;
    s.health = g_health;
    s.maxHealth = g_maxHealth;
    s.ticks = g_ticks;
    s.objectScans = g_scans;
    s.deaths = g_deaths;
    s.scriptEvents = g_scriptEvents;
    return s;
}

bool game::Ready()     { return g_ready; }
bool game::IsLoading() { return g_loading; }

void game::OnTick(TickCallback callback)
{
    std::lock_guard lock(g_mutex);
    g_tickCallbacks.push_back(callback);
}

void game::RunOnGameThread(std::function<void()> fn)
{
    if (!g_ready) return;                // Nothing would ever run it
    std::lock_guard lock(g_mutex);
    g_queue.push_back(std::move(fn));
}

// ---- lookups (same strategy as mmultiplayer: find the world that owns the local TdPlayerController) -----------------
Classes::ATdPlayerController* game::Controller()
{
    if (!g_ready || g_loading) return nullptr;
    if (g_validatedTick != g_ticks) ValidateCache();   // Called outside the tick hook (another hook, a callback)
    if (g_controller) return g_controller;
    if (g_ticks < g_nextScanTick) return nullptr;      // Looked a moment ago and there was none

    ++g_scans;
    const auto& objects = Classes::UObject::GetGlobalObjects();
    const auto* worldClass = Classes::AWorldInfo::StaticClass();
    const auto* controllerClass = Classes::ATdPlayerController::StaticClass();
    for (std::size_t i = 0; i < objects.Num() && !g_controller; ++i) {
        const auto object = objects.GetByIndex(i);
        if (!object || !IsA(object, worldClass)) continue;

        const auto world = static_cast<Classes::AWorldInfo*>(object);
        for (auto c = world->ControllerList; c; c = c->NextController) {
            if (!IsA(c, controllerClass)) continue;
            const auto pc = static_cast<Classes::ATdPlayerController*>(c);
            if (!pc->PlayerCamera) break;                 // Level not fully set up yet
            g_world = world;
            g_worldIndex = i;
            g_controller = pc;
            g_controllerIndex = IndexOf(pc);
            break;
        }
    }
    if (!g_controller) g_nextScanTick = g_ticks + kRescanTicks;
    return g_controller;
}

Classes::AWorldInfo* game::World()
{
    Controller();
    return g_world;
}

Classes::ATdPlayerPawn* game::Pawn()
{
    const auto controller = Controller();
    if (!controller || !controller->AcknowledgedPawn) return nullptr;
    if (!IsA(controller->AcknowledgedPawn, Classes::ATdPlayerPawn::StaticClass())) return nullptr;
    return static_cast<Classes::ATdPlayerPawn*>(controller->AcknowledgedPawn);
}
