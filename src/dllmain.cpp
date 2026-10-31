#include <Windows.h>
#include <spdlog/spdlog.h>

#include "config.h"
#include "core/clock.h"
#include "core/module.h"
#include "core/scheduler.h"
#include "diag.h"
#include "failsafe.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "selftest.h"
#include "test_mode.h"
#include "unload.h"
#if MEMENU_GAME
#include "game/game.h"
#include "game/levels.h"
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

    STARTUP_STEP(clk::Init());                // Before anything measures time (scheduler, caches)
    STARTUP_STEP(theme::Init());              // themes/ folder + built-in theme files
    STARTUP_STEP(modules::Init());            // Every module registered with MEM_MODULE
    STARTUP_STEP(modules::ApplyDefaults());   // Modules flagged enabledByDefault
    STARTUP_STEP(config::Load());             // Saved state overrides the defaults
    STARTUP_STEP(sched::Init());              // Run order of the modules' OnTick

    const bool installed = STARTUP_STEP(hooks::Install());
    if (installed)
    {
#if MEMENU_GAME
        // Module logic runs on the game thread through the engine tick; the menu itself stays on the render thread.
        const bool gameReady = STARTUP_STEP(game::Init());
        if (gameReady)
        {
            game::OnTick([](float) { modules::Tick(); });
            STARTUP_STEP(levels::Init());
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

        config::Save();         // Save before DisableAll so the enabled state is kept
        modules::DisableAll();  // Modules queue their cleanup on the game thread...
#if MEMENU_GAME
        game::Shutdown();       // ...which we wait for before the hooks go away
#endif
        hooks::Uninstall();
        if (selfTest) selftest::Finish(true);
    }
    else
    {
        config::Save();
        modules::DisableAll();
        if (selfTest) selftest::Finish(false);
    }

    spdlog::info("Unloading");
    testmode::Uninstall();
    diag::RemoveCrashLogger();
}

static DWORD WINAPI MainThread(LPVOID param)
{
    const auto module = static_cast<HMODULE>(param);
    paths::Init(module);
    logging::Init();
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
