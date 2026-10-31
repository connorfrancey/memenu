// memenu_testhost: loads medge_menu.dll into a small D3D9 program and drives it the way the game would, so a build can
// be tested without Mirror's Edge (`memenu verbose-build` runs it for every stage of the staged build).
//
//   memenu_testhost --dll <medge_menu.dll> --report <report.json> [--dumps <folder>] [--seconds <per phase>]
//
// Phases, in order (each is timed; a phase that runs too long counts as a hang):
//   window, device      a hidden window and a windowed D3D9 device (the game draws with D3D9 too)
//   baseline            frames before the menu is there (frame time to compare with)
//   load                LoadLibrary, in test mode (MEMENU_TEST_MODE=1: failed debug checks break instead of a dialog)
//   hook                frames until the menu has hooked EndScene (its first bytes change to a jump)
//   menu-closed         frames with the menu closed
//   menu-open           INSERT, then frames with the menu open and the mouse moving over it (every tab draws)
//   reset               IDirect3DDevice9::Reset to another size and back (the menu must survive a device reset)
//   menu-close          INSERT again, frames
//   unload              the unload event (Local\memenu-unload-<pid>), frames until the DLL is gone
//   after-unload        EndScene must be unhooked; frames (a hook left behind would crash here)
// The report (JSON) is rewritten after every phase, so it is there even when the process dies. Crashes and hangs
// write a minidump to --dumps and end the process (exit 3 = crash, 4 = hang). Exit 0 = every phase passed,
// 1 = a phase failed, 2 = bad arguments or setup.
#include <Windows.h>
#include <d3d9.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace {
    // ---- options & report ----------------------------------------------------------------------------------------
    std::wstring g_dll, g_reportPath, g_dumpDir;
    double g_seconds = 2.0;

    struct Phase { std::string name; bool ok; double ms; std::string detail; };
    std::mutex g_mutex;                         // Report state: written from the main, watchdog and crashing threads
    std::vector<Phase> g_phases;
    std::string g_result = "running", g_crash, g_metrics, g_frames;
    std::atomic<const char*> g_current{"start"};
    std::atomic<ULONGLONG> g_phaseStart{0};
    std::atomic<DWORD> g_phaseLimitMs{30000};
    DWORD g_mainThread = 0;                     // The "game" thread (frames); the menu has its own thread

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    std::string Json(const std::string& s)
    {
        std::string out = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                    else out += static_cast<char>(c);
            }
        }
        return out + "\"";
    }

    template <class... A>
    std::string Fmt(const char* fmt, A... args)
    {
        char buf[1024];
        std::snprintf(buf, sizeof buf, fmt, args...);
        return buf;
    }

    // Callers hold g_mutex
    void WriteReportLocked()
    {
        if (g_reportPath.empty()) return;
        std::string j = "{\n  \"dll\": " + Json(Utf8(g_dll)) + ",\n  \"pid\": " + std::to_string(GetCurrentProcessId()) +
                        ",\n  \"result\": " + Json(g_result) + ",\n  \"phase\": " + Json(g_current.load()) +
                        ",\n  \"phases\": [";
        for (std::size_t i = 0; i < g_phases.size(); ++i) {
            const Phase& p = g_phases[i];
            j += (i ? ",\n    " : "\n    ") + std::string("{\"name\": ") + Json(p.name) + ", \"ok\": " +
                 (p.ok ? "true" : "false") + ", \"ms\": " + Fmt("%.1f", p.ms) + ", \"detail\": " + Json(p.detail) + "}";
        }
        j += "\n  ]";
        if (!g_frames.empty()) j += ",\n  \"frames\": {" + g_frames + "}";
        if (!g_metrics.empty()) j += ",\n  \"metrics\": {" + g_metrics + "}";
        if (!g_crash.empty()) j += ",\n  \"crash\": {" + g_crash + "}";
        j += "\n}\n";

        const std::wstring tmp = g_reportPath + L".tmp";
        FILE* f = nullptr;
        if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return;
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), g_reportPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void WriteReport() { std::lock_guard lock(g_mutex); WriteReportLocked(); }

    // ---- crash & hang handling ------------------------------------------------------------------------------------
    std::string DescribeAddress(const void* addr, std::string* moduleOut = nullptr, std::uintptr_t* offsetOut = nullptr)
    {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(addr), &m))
            return Fmt("%p", addr);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(m, path, MAX_PATH);
        const auto off = reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(m);
        if (moduleOut) *moduleOut = Utf8(path);
        if (offsetOut) *offsetOut = off;
        const wchar_t* name = wcsrchr(path, L'\\');
        return Utf8(name ? name + 1 : path) + Fmt("+0x%IX", off);
    }

    std::wstring WriteDump(const char* kind, EXCEPTION_POINTERS* ep)
    {
        if (g_dumpDir.empty()) return {};
        CreateDirectoryW(g_dumpDir.c_str(), nullptr);
        const std::wstring path = g_dumpDir + L"\\testhost-" + std::wstring(kind, kind + std::strlen(kind)) + L"-" +
                                  std::to_wstring(GetCurrentProcessId()) + L".dmp";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
                                                     MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, ep ? &mei : nullptr,
                                          nullptr, nullptr);
        CloseHandle(file);
        return ok ? path : std::wstring();
    }

    LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
    {
        static std::atomic<bool> once{false};
        if (once.exchange(true)) TerminateProcess(GetCurrentProcess(), 3);
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        std::string module;
        std::uintptr_t offset = 0;
        const std::string where = DescribeAddress(er->ExceptionAddress, &module, &offset);
        const std::wstring dump = WriteDump("crash", ep);
        {
            std::lock_guard lock(g_mutex);
            g_result = "crash";
            g_crash = "\"code\": " + Json(Fmt("0x%08lX", er->ExceptionCode)) + ", \"address\": " + Json(where) +
                      ", \"module\": " + Json(module) + ", \"offset\": " + std::to_string(offset) +
                      ", \"thread\": " + std::to_string(GetCurrentThreadId()) +
                      ", \"main_thread\": " + (GetCurrentThreadId() == g_mainThread ? "true" : "false") +
                      ", \"dump\": " + Json(Utf8(dump));
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                g_crash += ", \"access\": " + Json(Fmt("%s 0x%08IX", er->ExceptionInformation[0] == 0 ? "read" :
                                                       er->ExceptionInformation[0] == 1 ? "write" : "execute",
                                                       er->ExceptionInformation[1]));
            WriteReportLocked();
        }
        std::fprintf(stderr, "testhost: crash 0x%08lX at %s in phase %s\n", er->ExceptionCode, where.c_str(), g_current.load());
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 3);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    DWORD WINAPI Watchdog(LPVOID)
    {
        for (;;) {
            Sleep(250);
            const ULONGLONG start = g_phaseStart.load();
            if (start && GetTickCount64() - start > g_phaseLimitMs.load()) {
                const std::wstring dump = WriteDump("hang", nullptr);
                {
                    std::lock_guard lock(g_mutex);
                    g_result = "hang";
                    g_crash = "\"hang\": true, \"limit_ms\": " + std::to_string(g_phaseLimitMs.load()) +
                              ", \"dump\": " + Json(Utf8(dump));
                    WriteReportLocked();
                }
                std::fprintf(stderr, "testhost: phase %s hung (> %lu ms)\n", g_current.load(), g_phaseLimitMs.load());
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 4);
            }
        }
    }

    // ---- phases ---------------------------------------------------------------------------------------------------
    void Begin(const char* name, DWORD limitMs)
    {
        g_current = name;
        g_phaseLimitMs = limitMs;
        g_phaseStart = GetTickCount64();
        std::printf("phase %s...\n", name);
        std::fflush(stdout);
        WriteReport();
    }

    bool End(bool ok, const std::string& detail = {})
    {
        const double ms = static_cast<double>(GetTickCount64() - g_phaseStart.load());
        g_phaseStart = 0;
        {
            std::lock_guard lock(g_mutex);
            g_phases.push_back({g_current.load(), ok, ms, detail});
            WriteReportLocked();
        }
        std::printf("phase %s: %s (%.0f ms)%s%s\n", g_current.load(), ok ? "ok" : "FAILED", ms,
                    detail.empty() ? "" : ": ", detail.c_str());
        std::fflush(stdout);
        return ok;
    }

    // ---- the D3D9 "game" ------------------------------------------------------------------------------------------
    HWND g_wnd = nullptr;
    IDirect3D9* g_d3d = nullptr;
    IDirect3DDevice9* g_dev = nullptr;
    D3DPRESENT_PARAMETERS g_pp{};
    double g_lastFrameMs = 0;

    void Pump()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {   // The menu hooks PeekMessage: go through it like the game
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    HRESULT Frame(int i)
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        Pump();
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 30 + i % 40, 60), 1.f, 0);
        g_dev->BeginScene();
        g_dev->EndScene();
        const HRESULT hr = g_dev->Present(nullptr, nullptr, nullptr, nullptr);
        QueryPerformanceCounter(&b);
        g_lastFrameMs = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        Sleep(1);
        return hr;
    }

    // Frames for `seconds` (with `each` called before every frame). Returns the average frame time.
    template <class F>
    double Frames(double seconds, F&& each)
    {
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
        double total = 0;
        int n = 0;
        while (GetTickCount64() < until) {
            each(n);
            Frame(n);
            total += g_lastFrameMs;
            ++n;
        }
        return n ? total / n : 0;
    }
    double Frames(double seconds) { return Frames(seconds, [](int) {}); }

    void* EndSceneAddress() { return (*reinterpret_cast<void***>(g_dev))[42]; }
    unsigned char FirstByte(void* fn) { unsigned char b = 0; SIZE_T r; ReadProcessMemory(GetCurrentProcess(), fn, &b, 1, &r); return b; }

    void PostKey(UINT vk)
    {
        const LPARAM lp = 1 | (static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16);
        PostMessageW(g_wnd, WM_KEYDOWN, vk, lp);
        PostMessageW(g_wnd, WM_KEYUP, vk, lp | (1u << 30) | (1u << 31));
    }

    struct Usage { DWORD handles = 0, threads = 0; SIZE_T privateBytes = 0; };
    Usage Measure()
    {
        Usage u;
        GetProcessHandleCount(GetCurrentProcess(), &u.handles);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == GetCurrentProcessId()) ++u.threads;
            CloseHandle(snap);
        }
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof pmc;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
            u.privateBytes = pmc.PrivateUsage;
        return u;
    }

    bool ParseArgs(int argc, wchar_t** argv)
    {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
            const wchar_t* v = nullptr;
            if (a == L"--dll" && (v = next())) g_dll = v;
            else if (a == L"--report" && (v = next())) g_reportPath = v;
            else if (a == L"--dumps" && (v = next())) g_dumpDir = v;
            else if (a == L"--seconds" && (v = next())) g_seconds = _wtof(v);
            else { std::fwprintf(stderr, L"unknown or incomplete option: %ls\n", a.c_str()); return false; }
        }
        if (g_dll.empty()) { std::fwprintf(stderr, L"usage: memenu_testhost --dll <path> [--report <json>] [--dumps <dir>] [--seconds <n>]\n"); return false; }
        if (g_seconds <= 0) g_seconds = 2.0;
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (!ParseArgs(argc, argv)) return 2;
    g_mainThread = GetCurrentThreadId();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);   // No WER dialogs
    SetUnhandledExceptionFilter(OnCrash);
    CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr));
    const DWORD frameLimit = static_cast<DWORD>(g_seconds * 1000) + 20000;
    bool ok = true;

    // ---- the "game" ---------------------------------------------------------------------------------------------
    Begin("window", 10000);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MemenuTestHost";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"memenu test host", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr,
                            nullptr, wc.hInstance, nullptr);   // Never shown: nothing pops up while testing
    if (!End(g_wnd != nullptr, g_wnd ? "" : Fmt("CreateWindowExW failed (error %lu)", GetLastError()))) return 2;

    Begin("device", 15000);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.hDeviceWindow = g_wnd;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.BackBufferWidth = 800;
    g_pp.BackBufferHeight = 600;
    HRESULT hr = g_d3d ? g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                             &g_pp, &g_dev) : E_FAIL;
    if (FAILED(hr) && g_d3d)
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_dev);
    if (!End(SUCCEEDED(hr), SUCCEEDED(hr) ? "" : Fmt("CreateDevice failed: hr=0x%08lX", static_cast<unsigned long>(hr)))) return 2;

    Begin("baseline", frameLimit);
    const double baseMs = Frames(0.5);
    End(true, Fmt("%.2f ms/frame", baseMs));
    const Usage before = Measure();

    // ---- load -----------------------------------------------------------------------------------------------------
    SetEnvironmentVariableW(L"MEMENU_TEST_MODE", L"1");
    wchar_t eventName[64];
    std::swprintf(eventName, 64, L"Local\\memenu-unload-%lu", GetCurrentProcessId());
    HANDLE unloadEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    void* endScene = EndSceneAddress();
    const unsigned char original = FirstByte(endScene);

    Begin("load", 30000);
    HMODULE dll = LoadLibraryW(g_dll.c_str());
    if (!End(dll != nullptr, dll ? DescribeAddress(dll) : Fmt("LoadLibraryW failed (error %lu)", GetLastError()))) {
        std::lock_guard lock(g_mutex);
        g_result = "failed";
        WriteReportLocked();
        return 1;
    }

    // ---- drive the menu -------------------------------------------------------------------------------------------
    Begin("hook", 30000);     // The menu retries its dummy device for ~10 s before giving up
    const ULONGLONG hookDeadline = GetTickCount64() + 25000;
    int n = 0;
    while (FirstByte(endScene) == original && GetTickCount64() < hookDeadline) Frame(n++);
    const bool hooked = FirstByte(endScene) != original;
    ok &= End(hooked, hooked ? Fmt("EndScene hooked after %d frames", n) : "EndScene was never hooked (25 s)");

    double closedMs = 0, openMs = 0, resetMs = 0;
    if (hooked) {
        Begin("menu-closed", frameLimit);
        closedMs = Frames(g_seconds);
        End(true, Fmt("%.2f ms/frame", closedMs));

        Begin("menu-open", frameLimit);
        PostKey(VK_INSERT);
        openMs = Frames(g_seconds, [](int i) {   // Sweep the mouse over the menu windows (hover, tooltips)
            const int x = 30 + (i * 13) % 740, y = 30 + (i * 7) % 540;
            PostMessageW(g_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        });
        End(true, Fmt("%.2f ms/frame", openMs));

        Begin("reset", frameLimit);
        D3DPRESENT_PARAMETERS pp = g_pp;
        pp.BackBufferWidth = 1024;
        pp.BackBufferHeight = 768;
        const HRESULT r1 = g_dev->Reset(&pp);
        resetMs = Frames(g_seconds / 2);
        pp = g_pp;
        const HRESULT r2 = g_dev->Reset(&pp);
        Frames(g_seconds / 2);
        ok &= End(SUCCEEDED(r1) && SUCCEEDED(r2), Fmt("Reset 1024x768: hr=0x%08lX, back to 800x600: hr=0x%08lX",
                                                      static_cast<unsigned long>(r1), static_cast<unsigned long>(r2)));

        Begin("menu-close", frameLimit);
        PostKey(VK_INSERT);
        Frames(g_seconds / 2);
        End(true);
    }

    // ---- unload ---------------------------------------------------------------------------------------------------
    Begin("unload", 40000);
    SetEvent(unloadEvent);
    const ULONGLONG unloadDeadline = GetTickCount64() + 30000;
    bool gone = false;
    n = 0;
    while (!gone && GetTickCount64() < unloadDeadline) {
        Frame(n++);   // Keep drawing while it unhooks, like the game does
        HMODULE m = nullptr;
        gone = !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, g_dll.c_str(), &m);
    }
    ok &= End(gone, gone ? Fmt("unloaded after %d frames", n) : "the DLL was still loaded 30 s after the unload request");

    Begin("after-unload", frameLimit);
    const bool restored = FirstByte(endScene) == original;
    const double afterMs = Frames(0.5);
    const Usage after = Measure();
    ok &= End(!gone || restored, restored ? Fmt("EndScene restored, %.2f ms/frame", afterMs)
                                          : "EndScene still hooked after the DLL unloaded");

    {
        std::lock_guard lock(g_mutex);
        g_frames = Fmt("\"baseline_ms\": %.3f, \"menu_closed_ms\": %.3f, \"menu_open_ms\": %.3f, \"after_reset_ms\": %.3f, "
                       "\"after_unload_ms\": %.3f", baseMs, closedMs, openMs, resetMs, afterMs);
        g_metrics = Fmt("\"handles_before\": %lu, \"handles_after\": %lu, \"threads_before\": %lu, \"threads_after\": %lu, "
                        "\"private_kb_before\": %Iu, \"private_kb_after\": %Iu",
                        before.handles, after.handles, before.threads, after.threads,
                        before.privateBytes / 1024, after.privateBytes / 1024);
        g_result = ok ? "ok" : "failed";
        g_current = "done";
        WriteReportLocked();
    }

    g_dev->Release();
    g_d3d->Release();
    DestroyWindow(g_wnd);
    CloseHandle(unloadEvent);
    return ok ? 0 : 1;
}
