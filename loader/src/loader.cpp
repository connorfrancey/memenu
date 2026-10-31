// dinput8.dll proxy: Mirror's Edge imports DINPUT8.dll and Windows looks next to the exe first, so a dinput8.dll in the
// game's Binaries folder is loaded with the game. It forwards DirectInput to the real system DLL and loads the menu from
// <Binaries>\memenu\medge_menu.dll once the game is up.
//
// "Up" = the game has a visible main window. Not earlier: the Steam executable is encrypted (SteamStub) until its entry
// point has run, and the menu pattern-scans the game's code and hooks Direct3D as soon as it is loaded.
#include <Windows.h>
#include <Unknwn.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// The installer finds this string in the file to tell our dinput8.dll from someone else's.
extern "C" const char MemenuLoaderMarker[] = "memenu-loader:" MEMENU_VERSION;

namespace
{
    constexpr DWORD kWindowTimeoutMs = 5 * 60 * 1000;   // Give up waiting for the game window after this long
    constexpr DWORD kSettleMs = 2000;                   // After the window shows up (same as the old launcher)

    HMODULE g_self = nullptr;
    std::wstring g_dir;                                 // Folder of this DLL (the game's Binaries folder)

    // ---- Log: <dir>\memenu\loader.log ---------------------------------------------------------------------------
    void Log(const char* fmt, ...)
    {
        const std::wstring path = g_dir + L"\\memenu\\loader.log";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(f, fmt, args);
        va_end(args);
        std::fputc('\n', f);
        std::fclose(f);
    }

    // ---- The real dinput8.dll -----------------------------------------------------------------------------------
    HMODULE RealDInput()
    {
        static HMODULE real = [] {
            wchar_t sys[MAX_PATH]{};
            GetSystemDirectoryW(sys, MAX_PATH);    // SysWOW64 for this 32-bit process (WOW64 redirects System32)
            HMODULE h = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());
            if (!h) Log("could not load the system dinput8.dll (error %lu)", GetLastError());
            return h;
        }();
        return real;
    }

    template <class Fn>
    Fn Real(const char* name)
    {
        HMODULE h = RealDInput();
        return h ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, name))) : nullptr;
    }

    // ---- Loading the menu ---------------------------------------------------------------------------------------
    struct WindowSearch { DWORD pid; bool found; };

    BOOL CALLBACK FindMainWindow(HWND hwnd, LPARAM lp)
    {
        auto* s = reinterpret_cast<WindowSearch*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == s->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
            s->found = true;
            return FALSE;
        }
        return TRUE;
    }

    bool WaitForGameWindow()
    {
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < kWindowTimeoutMs) {
            WindowSearch s{GetCurrentProcessId(), false};
            EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&s));
            if (s.found) return true;
            Sleep(250);
        }
        return false;
    }

    DWORD WINAPI LoaderThread(LPVOID)
    {
        const std::wstring menu = g_dir + L"\\memenu\\medge_menu.dll";
        if (GetFileAttributesW(menu.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log("%ls not found: nothing to load (reinstall with memenu_installer.exe)", menu.c_str());
            return 0;
        }
        // memenu\disabled: start the game without the menu, without uninstalling it
        if (GetFileAttributesW((g_dir + L"\\memenu\\disabled").c_str()) != INVALID_FILE_ATTRIBUTES) {
            Log("memenu\\disabled exists: not loading the menu");
            return 0;
        }

        if (!WaitForGameWindow()) {
            Log("the game window never appeared: not loading the menu");
            return 0;
        }
        Sleep(kSettleMs);

        if (LoadLibraryW(menu.c_str())) Log("loaded %ls", menu.c_str());
        else Log("LoadLibrary(%ls) failed (error %lu)", menu.c_str(), GetLastError());
        return 0;
    }

    bool IsGameProcess()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = wcsrchr(exe, L'\\');
        return _wcsicmp(name ? name + 1 : exe, L"MirrorsEdge.exe") == 0;
    }
}

// ---- dinput8 exports (see dinput8.def) ------------------------------------------------------------------------------
extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static const Fn real = Real<Fn>("DirectInput8Create");
    return real ? real(inst, version, riid, out, outer) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    using Fn = HRESULT(WINAPI*)();
    static const Fn real = Real<Fn>("DllCanUnloadNow");
    return real ? real() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    static const Fn real = Real<Fn>("DllGetClassObject");
    return real ? real(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    static const Fn real = Real<Fn>("DllRegisterServer");
    return real ? real() : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    static const Fn real = Real<Fn>("DllUnregisterServer");
    return real ? real() : E_FAIL;
}

extern "C" const void* WINAPI Proxy_GetdfDIJoystick()
{
    using Fn = const void*(WINAPI*)();
    static const Fn real = Real<Fn>("GetdfDIJoystick");
    return real ? real() : nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);

        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = path;
        g_dir.erase(g_dir.find_last_of(L'\\'));

        // Only the game itself: other programs that happen to load DirectInput from this folder get plain DirectInput.
        // The thread starts running once the loader lock is released, so LoadLibrary there is safe.
        if (IsGameProcess())
            if (HANDLE h = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
