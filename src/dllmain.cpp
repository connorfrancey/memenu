#include <Windows.h>
#include <spdlog/spdlog.h>

#include "config.h"
#include "core/clock.h"
#include "core/module.h"
#include "core/scheduler.h"
#include "game/game.h"
#include "game/levels.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "tas/tas.h"
#include "theme.h"

static DWORD WINAPI MainThread(LPVOID module)
{
    paths::Init(static_cast<HMODULE>(module));
    logging::Init();
    spdlog::info("MirrorsEdgeMenu attached");

    clk::Init();                // Before anything measures time (scheduler, caches)
    theme::Init();              // themes/ folder + built-in theme files
    modules::Init();            // Every module registered with MEM_MODULE
    modules::ApplyDefaults();   // Modules flagged enabledByDefault
    config::Load();             // Saved state overrides the defaults
    sched::Init();              // Run order of the modules' OnTick

    if (hooks::Install())
    {
        // Module logic runs on the game thread through the engine tick; the menu itself stays on the render thread.
        if (game::Init())
        {
            game::OnTick([](float) { modules::Tick(); });
            levels::Init();
            if (!tas::Init()) spdlog::warn("TAS unavailable: its patterns were not found");
        }
        else
        {
            spdlog::error("Game integration unavailable: the menu works, game modules do nothing");
        }

        while (!(GetAsyncKeyState(VK_END) & 1))
            Sleep(50);

        config::Save();         // Save before DisableAll so the enabled state is kept
        modules::DisableAll();  // Modules queue their cleanup on the game thread...
        game::Shutdown();       // ...which we wait for before the hooks go away
        hooks::Uninstall();
    }
    else
    {
        config::Save();
        modules::DisableAll();
    }

    spdlog::info("Unloading");
    logging::Shutdown();
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
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
