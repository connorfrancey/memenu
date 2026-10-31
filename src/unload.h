#pragma once
// When the menu should unload: END pressed, or the named event "Local\memenu-unload-<pid>" set by another program
// (the test host of `memenu verbose-build`, `memenu dev unload`). Header-only: the staged build's bare stages use it.
#include <Windows.h>

#include <cwchar>

namespace unload {
    inline HANDLE Event()
    {
        static HANDLE h = [] {
            wchar_t name[64];
            std::swprintf(name, 64, L"Local\\memenu-unload-%lu", GetCurrentProcessId());
            return CreateEventW(nullptr, TRUE, FALSE, name);   // Opens it when the test host made it first
        }();
        return h;
    }

    // A window of this process (the game) has the focus: GetAsyncKeyState sees keys pressed in every program, and
    // END typed in an editor or a chat must not unload the menu.
    inline bool GameFocused()
    {
        DWORD pid = 0;
        const HWND fg = GetForegroundWindow();
        return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    }

    // Poll from the menu thread. Also creates the event, so call it once early.
    inline bool Requested()
    {
        const bool endPressed = (GetAsyncKeyState(VK_END) & 1) != 0;   // Read every time: clears the "pressed" bit
        if (endPressed && GameFocused()) return true;
        const HANDLE h = Event();
        return h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    }
}
