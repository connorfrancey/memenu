#include <Windows.h>
#include <atomic>
#include <functional>
#include <spdlog/spdlog.h>

#include "config.h"
#include "core/clock.h"
#include "core/hardware.h"
#include "core/module.h"
#include "core/profile.h"
#include "core/scheduler.h"
#include "core/signature_cache.h"
#include "diag.h"
#include "failsafe.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "selftest.h"
#include "core/devmode.h"
#include "test_mode.h"
#include "unload.h"
#if MEMENU_GAME
#include "game/game.h"
#include "game/levels.h"
#include "runtime/mods.h"
#endif
#if MEMENU_FEATURE_TAS
#include "tas/tas.h"
#endif
#include "theme.h"

// Logs each startup step and how long it took, so a hang or crash shows which step it was in.
template <class F>
static auto Step(const char* name, F&& f)
{
    spdlog::info("Startup: {}...", name);
    const ULONGLONG start = GetTickCount64();
    struct Done {
        const char* name; ULONGLONG start;
        ~Done() { spdlog::info("Startup: {} done ({} ms)", name, GetTickCount64() - start); }
    } done{name, start};
    return f();
}
#define STARTUP_STEP(call) Step(#call, [&] { return call; })

static void Run(HMODULE module)
{
    const ULONGLONG attachedAt = GetTickCount64();
    spdlog::info("MirrorsEdgeMenu attached{}", testmode::Enabled() ? " (test mode: failed checks break instead of a dialog)" : "");
    diag::LogStartup(module);
    diag::InstallCrashLogger(module);
    testmode::Install([](const char* message) {
        spdlog::critical("Debug check failed: {}", message);
        spdlog::default_logger()->flush();
    });
    unload::Requested();   // Creates the unload event before anything can go wrong
    const bool selfTest = selftest::Requested();   // `selftest` file next to the DLL: see selftest.h
    if (selfTest) diag::LogGameCrashesUntil(~0ull);    // Every crash, the game's too, with its stack
    if (selfTest) config::SetAutoSave(false);      // The test changes settings: never into the player's config

    STARTUP_STEP(clk::Init());                // Before anything measures time (scheduler, caches)
    prof::Start();                            // Tracy builds only: profiler threads (stopped after the hooks are gone)
    STARTUP_STEP(theme::Init());              // themes/ folder + built-in theme files
    STARTUP_STEP(modules::Init());            // Every module registered with MEM_MODULE
#if MEMENU_GAME
    // Mirage mods (mods\<id>\mod.toml): found and ordered now, the early phase loaded before the config and the
    // scheduler see the modules. The self-test also loads its test mods (selftest-mods, put there by `memenu game-test`).
    STARTUP_STEP(mirage::mods::Discover(selfTest ? std::vector{paths::Dir() / "mods", paths::Dir() / "selftest-mods"}
                                                 : std::vector{paths::Dir() / "mods"}));
    STARTUP_STEP(mirage::mods::LoadPhase(mirage::mods::Phase::Early));
    diag::SetCrashAttributor([](const void* address, unsigned long code) { return mirage::mods::AttributeCrash(address, code); });
#endif
    STARTUP_STEP(modules::ApplyDefaults());   // Modules flagged enabledByDefault
    STARTUP_STEP(config::Load());             // Saved state overrides the defaults
    devmode::ApplyBuild();                    // A developer build (memenu --dev) switches developer mode on
    STARTUP_STEP(sched::Init());              // Run order of the modules' OnTick

    const bool installed = STARTUP_STEP(hooks::Install());
    spdlog::info("Hardware: {}", hw::Get().Summary());   // After the hooks: they add the GPU
    if (installed)
    {
        // From here modules added later (mods of the engine and world phases, hot reloads) join on the render thread,
        // with their saved state and a place in the scheduler
        modules::Start([](std::function<void()> fn) { hooks::RunOnRenderThread(std::move(fn)); }, [](Module& m) {
            if (!config::ApplySaved(m) && m.enabledByDefault) m.SetEnabled(true);
            sched::AddModule(m);
        });
#if MEMENU_GAME
        // Module logic runs on the game thread through the engine tick; the menu itself stays on the render thread.
        const bool gameReady = STARTUP_STEP(game::Init());
        if (gameReady)
        {
            // The world phase's mods load on the first tick after a level has loaded
            static std::atomic<bool> levelLoaded{false}, worldLoaded{false};
            game::OnLevelLoad([](bool finished) { if (finished) levelLoaded = true; });
            game::OnTick([](float) {
                if (levelLoaded && !worldLoaded.exchange(true)) mirage::mods::LoadPhase(mirage::mods::Phase::World);
                modules::Tick();
            });
            STARTUP_STEP(levels::Init());
            STARTUP_STEP(mirage::mods::LoadPhase(mirage::mods::Phase::Engine));   // The game layer is up
            // Hot reload: mods whose files changed, checked twice a second (game thread, between the modules' ticks)
            sched::AddTask("Mirage mod reload", {0.5f, sched::Priority::Background, {}, {}}, [] { mirage::mods::ReloadChanged(); });
#if MEMENU_FEATURE_TAS
            const bool tasReady = STARTUP_STEP(tas::Init());
            if (!tasReady) spdlog::warn("TAS unavailable: its patterns were not found");
#endif
        }
        else
        {
            spdlog::error("Game integration unavailable: the menu works, game modules do nothing");
        }
#else
        spdlog::info("Built without game integration (no game features enabled): menu only");
#endif

        sig::Game().Save();   // The pattern scans of startup: the next start skips them
        spdlog::info("Startup finished in {} ms; waiting for the game to draw a frame (EndScene)",
                     GetTickCount64() - attachedAt);

        // The hooks are on the game's D3D9 device methods: if EndScene never runs, the vtable came from a different
        // d3d9 than the one the game draws with (a wrapper/overlay), or the game is not rendering (minimized).
        const ULONGLONG installedAt = GetTickCount64();
        bool warnedNoFrames = false;
        if (selfTest) selftest::Run();   // Returns when the test is done: unload then
        while (!selfTest && !unload::Requested())
        {
            Sleep(50);
            if (!warnedNoFrames && hooks::EndSceneCalls() == 0 && GetTickCount64() - installedAt > 10000)
            {
                warnedNoFrames = true;
                spdlog::error("EndScene hook has not run 10 s after installing: the menu cannot draw. The game is "
                              "minimized/not rendering, or draws through another d3d9 (see the Module lines above)");
            }
        }
        spdlog::info("Unload requested: unloading (EndScene ran {} times)", hooks::EndSceneCalls());
        selftest::Shutdown();   // A run started from the Test module: stopped, settings put back (needs the hooks)

        config::Save();         // Save before DisableAll so the enabled state is kept
        modules::DisableAll();  // Modules queue their cleanup on the game thread...
#if MEMENU_GAME
        game::Shutdown();       // ...which we wait for before the hooks go away
#endif
#if MEMENU_FEATURE_TAS
        tas::Shutdown();        // The snapshot workers
#endif
        hooks::Uninstall();
#if MEMENU_GAME
        mirage::mods::UnloadAll();   // Nothing calls into a mod any more
#endif
        if (selfTest) selftest::Finish(true);
    }
    else
    {
        config::Save();
        modules::DisableAll();
#if MEMENU_GAME
        mirage::mods::UnloadAll();
#endif
        if (selfTest) selftest::Finish(false);
    }

    prof::Stop();           // After the hooks: nothing records zones any more
    spdlog::info("Unloading");
    testmode::Uninstall();
    diag::RemoveCrashLogger();
}

// One menu per game process. Two copies (the installed one from the game's loader plus an injected build) both hook
// the frame timing, input and D3D9: neither TAS works and both menus fight over the mouse. The launcher's "already
// loaded" check can't catch two copies arriving at the same moment, so the first one to get here owns the process.
// False when another copy holds the process. `claim` is closed at unload (nullptr if the mutex couldn't be created at
// all: then nothing is guarded, and this copy runs anyway).
static bool ClaimProcess(HANDLE& claim)
{
    wchar_t name[64];
    swprintf_s(name, L"Local\\MirrorsEdgeMenu-%lu", GetCurrentProcessId());
    claim = CreateMutexW(nullptr, FALSE, name);
    if (claim && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(claim);
        claim = nullptr;
        return false;
    }
    return true;
}

// Unloads this copy if another one owns the process (not inline in MainThread: __try can't share it with objects).
static HANDLE ClaimOrLeave(HMODULE module)
{
    HANDLE claim = nullptr;
    if (ClaimProcess(claim)) return claim;
    spdlog::error("Another copy of the menu is already running in this game: this one ({}) does nothing and unloads. "
                  "Use one copy: the installed one, or an injected build with the installed one disabled",
                  diag::ModulePath(module));
    OutputDebugStringW(L"MirrorsEdgeMenu: another copy is already running in this process; not starting\n");
    logging::Shutdown();
    FreeLibraryAndExitThread(module, 0);
}

static DWORD WINAPI MainThread(LPVOID param)
{
    const auto module = static_cast<HMODULE>(param);
    paths::Init(module);
    logging::Init();
    HANDLE claim = ClaimOrLeave(module);

    failsafe::Start([](const char* message) {   // First: it must work even if anything after this hangs
        spdlog::critical("{}", message);
        spdlog::default_logger()->flush();
    });
    spdlog::info("Failsafe: {} force-quits the game, even when it stops responding", failsafe::kName);
#ifdef _MSC_VER
    __try { Run(module); }
    __except (diag::LogFatal(GetExceptionInformation(), "Menu thread")) {}   // Logs, then the crash goes on as before
#else
    Run(module);
#endif
    failsafe::Stop();
    logging::Shutdown();
    if (claim) CloseHandle(claim);   // Last: a copy injected after this unload may run
    FreeLibraryAndExitThread(module, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE h = CreateThread(nullptr, 0, MainThread, module, 0, nullptr))
            CloseHandle(h);
    }
    return TRUE;
}
