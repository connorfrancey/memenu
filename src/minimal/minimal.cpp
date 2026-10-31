// The minimal build (MEMENU_MINIMAL, `memenu minimal`): only what an injected ImGui overlay strictly needs.
// The D3D9 / input hooks are the normal ones (hooks.cpp); this file replaces dllmain.cpp and gui.cpp with the
// ImGui demo window, toggled with INSERT. No modules, config or game integration.
// Used for the staged build's first stages (MEMENU_MINIMAL_STAGE): 0 = bare, 1 = + logging, 2 = + themes. From
// stage 3 on the real menu (dllmain.cpp, gui.cpp) is built instead.
#include "gui.h"
#include "hooks.h"
#include "failsafe.h"
#include "paths.h"
#include "test_mode.h"
#include "unload.h"

#include <Windows.h>
#include <imgui.h>

#if MEMENU_MINIMAL_STAGE >= 1
#include "log.h"
#include <spdlog/spdlog.h>
#define MINIMAL_LOG(msg) spdlog::info(msg)
#else
#define MINIMAL_LOG(msg) OutputDebugStringA("[memenu] " msg "\n")
#endif
#if MEMENU_MINIMAL_STAGE >= 2
#include "theme.h"
#endif
#define MINIMAL_STR2(x) #x
#define MINIMAL_STAGE_STR MINIMAL_STR2(MEMENU_MINIMAL_STAGE)

void gui::ApplyStyle()
{
#if MEMENU_MINIMAL_STAGE >= 2
    theme::ApplyBaseStyle();             // Same as the menu: base style, then the selected theme file on top
    theme::Apply(theme::Current());
#else
    ImGui::StyleColorsDark();
#endif
}

bool gui::CaptureKey(int) { return false; }   // No key binding UI

void gui::Render()
{
    if (!visible) return;
#if MEMENU_MINIMAL_STAGE >= 2
    theme::Begin();
#endif
    ImGui::ShowDemoWindow(&visible);     // Its close button hides it too; INSERT brings it back
#if MEMENU_MINIMAL_STAGE >= 2
    theme::End();
#endif
}

static DWORD WINAPI MainThread(LPVOID module)
{
    paths::Init(static_cast<HMODULE>(module));   // imgui.ini (and logs/) go next to the DLL
#if MEMENU_MINIMAL_STAGE >= 1
    logging::Init();
#endif
#if MEMENU_MINIMAL_STAGE >= 2
    theme::Init();                               // themes/ next to the DLL, built-in theme files
#endif
#if MEMENU_MINIMAL_STAGE >= 1
    failsafe::Start([](const char* message) {
        spdlog::critical("{}", message);
        spdlog::default_logger()->flush();
    });
#else
    failsafe::Start([](const char* message) { OutputDebugStringA(message); });
#endif
    MINIMAL_LOG("Minimal build attached (stage " MINIMAL_STAGE_STR "; INSERT: demo window, END: unload, "
                "Ctrl+Alt+Shift+F12: force-quit the game)");
#if MEMENU_MINIMAL_STAGE >= 1
    testmode::Install([](const char* message) {
        spdlog::critical("Debug check failed: {}", message);
        spdlog::default_logger()->flush();
    });
#else
    testmode::Install([](const char* message) { OutputDebugStringA(message); });
#endif
    unload::Requested();   // Creates the unload event

    if (hooks::Install())
    {
        while (!unload::Requested())
            Sleep(50);
        hooks::Uninstall();
    }

    MINIMAL_LOG("Unloading");
    testmode::Uninstall();
    failsafe::Stop();
#if MEMENU_MINIMAL_STAGE >= 1
    logging::Shutdown();
#endif
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
