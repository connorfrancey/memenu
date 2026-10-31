#include "game.h"
#include "core/cache.h"

#include <Windows.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <ME_SDK.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
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
    std::atomic<float> g_engineNear{0.f};       // From the engine's projection matrix, 0 = unknown
    std::atomic<bool> g_hasPawn{false};
    std::atomic<int> g_health{0}, g_maxHealth{0};

    std::mutex g_mutex;                         // Guards the two vectors below
    std::vector<game::TickCallback> g_tickCallbacks;
    std::vector<std::function<void()>> g_queue;
    std::vector<game::LevelLoadCallback> g_levelLoadCallbacks;
    std::vector<game::StateChangeCallback> g_stateChangeCallbacks;   // Not called yet: no hook for it

    // Cached lookups, game thread only. Cleared whenever a level starts loading.
    Classes::AWorldInfo* g_world = nullptr;
    Classes::ATdPlayerController* g_controller = nullptr;

    void (__thiscall *oTick)(float*, int, float) = nullptr;
    int (__thiscall *oLevelLoad)(void*, void*, unsigned long long) = nullptr;
    int* (__thiscall *oProjection)(float*, void*) = nullptr;

    // ---- Pattern scan over the main executable's image --------------------------------------------------------------
    // Pattern/mask as used by mmultiplayer: 'x' = byte must match, '?' = wildcard.
    void* FindPattern(const char* pattern, const char* mask)
    {
        const auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
        const std::size_t len = std::strlen(mask);

        for (std::size_t off = 0; off + len <= imageSize;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(base + off, &mbi, sizeof mbi)) break;

            const auto regionStart = static_cast<std::size_t>(static_cast<std::uint8_t*>(mbi.BaseAddress) - base);
            const std::size_t regionEnd = regionStart + mbi.RegionSize;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

            if (readable) {
                for (std::size_t i = off; i + len <= regionEnd; ++i) {
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
            // Look the world/controller up every tick (lazily, on first use): a respawn or level restart can
            // replace them without going through the level-load hook, and a stale pointer would be a crash.
            g_world = nullptr;
            g_controller = nullptr;

            DrainQueue();
            ApplyInputBlock();

            std::vector<game::TickCallback> callbacks;
            {
                std::lock_guard lock(g_mutex);
                callbacks = g_tickCallbacks;
            }
            for (auto cb : callbacks) cb(delta);

            if (g_ticks % 15 == 0) UpdateStatus();
        }
        oTick(scales, arg, delta);

        // The camera is read *after* the engine has ticked: the player controller updates its camera during the tick, so a
        // read before it is one frame old and a volume drawn from it trails the world as you move (jitter).
        if (!g_loading && g_trackCamera) {
            g_world = nullptr;
            g_controller = nullptr;     // The tick may have replaced them
            UpdateCamera();
        }
    }

    // The engine builds its projection matrix here (found by mmultiplayer). Only the near plane is taken from it, to make
    // the depth values of the debug renderer line up with the depth buffer of the game.
    int* __fastcall ProjectionHook(float* m, void* /*edx*/, void* arg) {
        int* ret = oProjection(m, arg);
        static bool logged = false;
        if (m && !IsBadReadPtr(m, 16 * sizeof(float))) {
            if (!logged) {
                logged = true;
                spdlog::info("Engine projection (raw): [{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] "
                             "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}]",
                             m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
            // Infinite-far perspective: [2][2] = 1 and [3][2] = -near
            const float z = m[10], w = m[14];
            if (std::isfinite(z) && std::isfinite(w) && std::fabs(z - 1.f) < 0.01f && w < -1.f && w > -200.f)
                g_engineNear = -w;
        }
        return ret;
    }

    int __fastcall LevelLoadHook(void* self, void* /*edx*/, void** levelInfo, unsigned long long arg)
    {
        g_loading = true;
        g_world = nullptr;
        g_controller = nullptr;
        cache::NextLevel();

        std::vector<game::LevelLoadCallback> callbacks;
        {
            std::lock_guard lock(g_mutex);
            callbacks = g_levelLoadCallbacks;
        }
        for (auto cb : callbacks) cb(false);
        const int ret = oLevelLoad(self, levelInfo, arg);
        g_loading = false;
        for (auto cb : callbacks) cb(true);
        return ret;
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

    // Optional: the debug renderer falls back to its own near plane setting without it.
    ptr = FindPattern("\x83\xEC\x3C\xD9\x44\x24\x44", "xxxxxxx");
    if (!ptr || !HookAt(ptr, reinterpret_cast<void*>(&ProjectionHook), reinterpret_cast<void**>(&oProjection), "Projection"))
        spdlog::warn("Projection pattern not found or not hookable: the debug renderer will use its near plane setting");

    g_ready = true;
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
    // TODO: hook the player's move state handler (mmultiplayer's trainer has the pattern). Until then nothing calls these.
    static bool warned = false;
    if (!warned) {
        warned = true;
        spdlog::warn("Player state change hook is not implemented: tools that rely on it (Strang) do nothing");
    }
    std::lock_guard lock(g_mutex);
    g_stateChangeCallbacks.push_back(callback);
}

void* game::FindPattern(const char* pattern, const char* mask)
{
    return ::FindPattern(pattern, mask);
}

void game::SetCameraTracking(bool enabled) {
    g_trackCamera = enabled;
}

float game::EngineNear() {
    return g_engineNear;
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

    if (!g_controller) {
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num() && !g_controller; ++i) {
            const auto object = objects.GetByIndex(i);
            if (!object || !object->IsA(Classes::AWorldInfo::StaticClass())) continue;

            const auto world = static_cast<Classes::AWorldInfo*>(object);
            for (auto c = world->ControllerList; c; c = c->NextController) {
                if (!c->IsA(Classes::ATdPlayerController::StaticClass())) continue;
                const auto pc = static_cast<Classes::ATdPlayerController*>(c);
                if (!pc->PlayerCamera) break;                 // Level not fully set up yet
                g_world = world;
                g_controller = pc;
                break;
            }
        }
    }

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
    if (!controller->AcknowledgedPawn->IsA(Classes::ATdPlayerPawn::StaticClass())) return nullptr;
    return static_cast<Classes::ATdPlayerPawn*>(controller->AcknowledgedPawn);
}
