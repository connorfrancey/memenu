#pragma once
// Failsafe: Ctrl+Alt+Shift+F12 force-quits the game (TerminateProcess on the process the menu is loaded in).
//
// It runs on its own thread with its own message loop, so it still works when the game's threads hang ("Not
// Responding", a frozen fullscreen frame): nothing it needs runs on them. The key goes through RegisterHotKey, which
// works while any window has focus; if another program already owns that hotkey it falls back to polling the keys.
// The kill path takes no locks the game could hold: logging gets 300 ms on a separate thread, then the process ends
// regardless. Header-only: the staged build's bare stages use it too.
// Not covered: a process that is suspended, or so broken that no thread of it runs (then use Task Manager).
#include <Windows.h>

namespace failsafe {
    using LogFn = void (*)(const char* message);
    constexpr UINT kMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    constexpr UINT kKey = VK_F12;
    constexpr const char* kName = "Ctrl+Alt+Shift+F12";
    constexpr UINT kExitCode = 0xDEAD;   // Exit code of a failsafe kill (57005), recognisable in crash tools

    struct State {
        HANDLE thread = nullptr;
        HANDLE ready = nullptr;     // Set once the thread has its message queue (Stop's WM_QUIT can't get lost)
        DWORD threadId = 0;
        LogFn log = nullptr;
    };
    inline State& S() { static State s; return s; }

    inline bool ComboDown()
    {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        return down(VK_CONTROL) && down(VK_MENU) && down(VK_SHIFT) && down(kKey);
    }

    inline DWORD WINAPI LogKill(LPVOID p)
    {
        char exe[MAX_PATH] = "?";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        char msg[MAX_PATH + 128];
        wsprintfA(msg, "Failsafe: %s pressed, terminating %s (pid %lu)", kName, exe, GetCurrentProcessId());
        reinterpret_cast<LogFn>(p)(msg);
        return 0;
    }

    [[noreturn]] inline void Kill()
    {
        if (LogFn log = S().log) {
            // Log on a separate thread: if a hung thread holds the logger's lock, this one must not wait for it
            HANDLE t = CreateThread(nullptr, 0, LogKill, reinterpret_cast<LPVOID>(log), 0, nullptr);
            if (t) { WaitForSingleObject(t, 300); CloseHandle(t); }
        }
        TerminateProcess(GetCurrentProcess(), kExitCode);
        ExitProcess(kExitCode);   // Not reached
    }

    inline DWORD WINAPI Thread(LPVOID)
    {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // Creates this thread's message queue
        SetEvent(S().ready);
        const bool registered = RegisterHotKey(nullptr, 1, kMods | MOD_NOREPEAT, kKey) != FALSE;
        if (!registered && S().log) S().log("Failsafe: Ctrl+Alt+Shift+F12 is taken by another program; watching the keys instead");

        for (;;) {
            if (registered) {
                const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
                if (r == 0 || r == -1) break;                         // WM_QUIT (Stop) or error
                if (msg.message == WM_HOTKEY && msg.wParam == 1) Kill();
            } else {
                if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0) {
                    bool quit = false;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) quit |= msg.message == WM_QUIT;
                    if (quit) break;
                }
                if (ComboDown()) Kill();
            }
        }
        if (registered) UnregisterHotKey(nullptr, 1);
        return 0;
    }

    // Call once, early (before anything that could hang). `log` gets the kill message, best effort.
    inline void Start(LogFn log)
    {
        State& s = S();
        if (s.thread) return;
        s.log = log;
        s.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s.thread = CreateThread(nullptr, 0, Thread, nullptr, 0, &s.threadId);
        if (s.thread && s.ready) WaitForSingleObject(s.ready, 2000);
    }

    // Before the DLL unloads: the thread's code is in this DLL.
    inline void Stop()
    {
        State& s = S();
        if (!s.thread) return;
        PostThreadMessageW(s.threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(s.thread, 2000);
        CloseHandle(s.thread);
        if (s.ready) CloseHandle(s.ready);
        s.thread = s.ready = nullptr;
        s.log = nullptr;
    }
}
