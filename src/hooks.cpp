#include "hooks.h"
#include "gui.h"
#if !MEMENU_MINIMAL
#include "core/module.h"
#endif
#if MEMENU_GAME
#include "render/debug_renderer.h"
#endif
#include "paths.h"

#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>
#if MEMENU_MINIMAL && MEMENU_MINIMAL_STAGE < 1
#define HOOKS_LOG(level, fmt, ...) OutputDebugStringA("[memenu] " fmt "\n")   // No spdlog: unformatted, to the debugger
#define HOOKS_DIAG 0
#else
#include <spdlog/spdlog.h>
#include "diag.h"
#define HOOKS_LOG(level, ...) spdlog::level(__VA_ARGS__)
#define HOOKS_DIAG 1   // The diagnostics below need spdlog/fmt
#endif
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#if MEMENU_GAME
#include "game/game.h"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
    using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using StretchRect_t = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                           const RECT*, D3DTEXTUREFILTERTYPE);

    EndScene_t oEndScene = nullptr;
    Reset_t oReset = nullptr;
#if MEMENU_GAME
    StretchRect_t oStretchRect = nullptr;
#endif
    WNDPROC oWndProc = nullptr;
    HWND g_hwnd = nullptr;
    bool g_imguiReady = false;
    std::atomic<unsigned> g_endSceneCalls{0};   // hooks::EndSceneCalls(): proves the hook fires

    std::mutex g_renderQueueMutex;              // hooks::RunOnRenderThread
    std::vector<std::function<void()>> g_renderQueue;

    void DrainRenderQueue()
    {
        std::vector<std::function<void()>> work;
        {
            std::lock_guard lock(g_renderQueueMutex);
            work.swap(g_renderQueue);
        }
        for (auto& fn : work) fn();
    }

#if HOOKS_DIAG
    // ---------------------------------------------------------------------------------------------------------------
    // Diagnostics: why the hooks could not be installed, or why they never fire.

    using diag::DescribeAddress;
    using diag::Narrow;

    // First bytes of a function. A jump there means someone (Steam/Discord/RTSS overlay, ReShade, a copy of this menu
    // that was not unloaded cleanly) already hooked it.
    std::string DescribePrologue(const void* fn)
    {
        unsigned char b[8]{};
        SIZE_T read = 0;
        if (!fn || !ReadProcessMemory(GetCurrentProcess(), fn, b, sizeof b, &read) || read != sizeof b) return "unreadable";
        std::string out;
        for (unsigned char c : b) out += fmt::format("{:02X} ", c);
        out.pop_back();
        if (b[0] == 0xE9)   // jmp rel32
        {
            std::int32_t rel;
            memcpy(&rel, b + 1, sizeof rel);
            out += " -> already hooked, jmp to " + DescribeAddress(static_cast<const unsigned char*>(fn) + 5 + rel);
        }
        else if (b[0] == 0xFF && b[1] == 0x25)   // jmp [abs]
        {
            void* slot;
            memcpy(&slot, b + 2, sizeof slot);
            void* target = nullptr;
            ReadProcessMemory(GetCurrentProcess(), slot, &target, sizeof target, &read);
            out += " -> already hooked, jmp to " + DescribeAddress(target);
        }
        else if (b[0] == 0x68 && b[5] == 0xC3)   // push imm32; ret
        {
            void* target;
            memcpy(&target, b + 1, sizeof target);
            out += " -> already hooked, push/ret to " + DescribeAddress(target);
        }
        return out;
    }

    const char* HResultName(HRESULT hr)
    {
        switch (hr)
        {
            case D3D_OK:                     return "D3D_OK";
            case D3DERR_DEVICELOST:          return "D3DERR_DEVICELOST (another device has exclusive fullscreen, or the session is locked)";
            case D3DERR_DEVICENOTRESET:      return "D3DERR_DEVICENOTRESET";
            case D3DERR_NOTAVAILABLE:        return "D3DERR_NOTAVAILABLE (device type/behaviour not supported)";
            case D3DERR_INVALIDCALL:         return "D3DERR_INVALIDCALL (bad parameters, e.g. no window)";
            case D3DERR_OUTOFVIDEOMEMORY:    return "D3DERR_OUTOFVIDEOMEMORY";
            case D3DERR_DRIVERINTERNALERROR: return "D3DERR_DRIVERINTERNALERROR";
            case E_OUTOFMEMORY:              return "E_OUTOFMEMORY";
            case E_FAIL:                     return "E_FAIL";
            default:                         return "unknown";
        }
    }

    std::string WindowText(HWND h, bool className)
    {
        wchar_t buf[256]{};
        if (className) GetClassNameW(h, buf, 256);
        else GetWindowTextW(h, buf, 256);
        return Narrow(buf);
    }

    // The game's main window: the largest visible top-level window of this process
    BOOL CALLBACK FindLargestWindow(HWND h, LPARAM lp)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r{};
        GetWindowRect(h, &r);
        auto& best = *reinterpret_cast<std::pair<HWND, long>*>(lp);
        const long area = (r.right - r.left) * (r.bottom - r.top);
        if (!best.first || area > best.second) best = {h, area};
        return TRUE;
    }

    void LogWindow(const char* what, HWND h)
    {
        if (!h) { HOOKS_LOG(warn, "{}: none (no visible window in the game process)", what); return; }
        RECT r{}, c{};
        GetWindowRect(h, &r);
        GetClientRect(h, &c);
        const LONG style = GetWindowLongW(h, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        const bool coversMonitor = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                                   r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
        HOOKS_LOG(info, "{}: hwnd={} class='{}' title='{}' window={}x{} at ({},{}) client={}x{} style=0x{:08X}{}{}{}{}",
                  what, static_cast<void*>(h), WindowText(h, true), WindowText(h, false), r.right - r.left,
                  r.bottom - r.top, r.left, r.top, c.right, c.bottom, static_cast<unsigned long>(style),
                  (style & WS_POPUP) ? " popup" : "", IsIconic(h) ? " MINIMIZED" : "",
                  GetForegroundWindow() == h ? " foreground" : " background",
                  coversMonitor ? " covers-monitor (fullscreen or borderless)" : " windowed");
    }

    void LogGameWindow()
    {
        std::pair<HWND, long> best{nullptr, 0};
        EnumWindows(FindLargestWindow, reinterpret_cast<LPARAM>(&best));
        LogWindow("Game window", best.first);
    }

    // Which d3d9.dll the game uses (a copy outside System32/SysWOW64 is a wrapper: ReShade, DXVK, dgVoodoo, ENB...)
    // and the adapter/display mode the dummy device is created on.
    void LogD3D9Environment(IDirect3D9* d3d)
    {
        if (HMODULE m = GetModuleHandleW(L"d3d9.dll"))
            HOOKS_LOG(info, "d3d9.dll: {} (base {})", diag::ModulePath(m), static_cast<void*>(m));
        if (!d3d) return;

        HOOKS_LOG(info, "D3D9 adapters: {}", d3d->GetAdapterCount());
        D3DADAPTER_IDENTIFIER9 id{};
        if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id)))
            HOOKS_LOG(info, "Default adapter: {} (driver {} {}.{}.{}.{})", id.Description, id.Driver,
                      HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
                      HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart));
        D3DDISPLAYMODE mode{};
        const HRESULT hr = d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
        if (SUCCEEDED(hr))
            HOOKS_LOG(info, "Display mode: {}x{} @ {} Hz, format {}", mode.Width, mode.Height, mode.RefreshRate,
                      static_cast<int>(mode.Format));
        else
            HOOKS_LOG(warn, "GetAdapterDisplayMode failed: hr=0x{:08X} {}", static_cast<unsigned long>(hr), HResultName(hr));
    }
#endif

    using PeekMessageW_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    using PeekMessageA_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    PeekMessageW_t oPeekMessageW = nullptr;
    PeekMessageA_t oPeekMessageA = nullptr;

    // Fresh key press: "press a key to bind", the menu toggle, module hotkeys. True if the key was consumed.
    bool OnKeyDown(WPARAM wp)
    {
        if (gui::CaptureKey(static_cast<int>(wp))) return true;
        if (wp == VK_INSERT)
        {
            gui::visible = !gui::visible;
            HOOKS_LOG(info, "Menu {}", gui::visible ? "opened" : "closed");
        }
#if !MEMENU_MINIMAL
        else modules::OnKey(static_cast<int>(wp));
#endif
        return false;
    }

    // Input the game must not see while the menu is open. Releases (key/button up) are let through so nothing
    // stays "held" in the game when the menu opens or closes in the middle of a press.
    bool IsBlockedWhileMenuOpen(UINT m)
    {
        switch (m)
        {
            case WM_INPUT:  // Raw mouse movement/buttons: this is what turns the camera
            case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
            case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_UNICHAR:
                return true;
            default:
                return false;
        }
    }

    // The game pumps its window with PeekMessage, so this is the point where its mouse/keyboard input can be cut
    // off while the menu is open: ImGui gets every message first, then the input ones are turned into WM_NULL.
    BOOL FilterPeeked(BOOL ret, LPMSG msg, UINT remove)
    {
        if (!ret || !msg || !(remove & PM_REMOVE) || !g_imguiReady || !gui::visible || msg->hwnd != g_hwnd) return ret;

        ImGui_ImplWin32_WndProcHandler(msg->hwnd, msg->message, msg->wParam, msg->lParam);

        if (msg->message == WM_KEYDOWN && !(msg->lParam & (1 << 30)))
        {
            OnKeyDown(msg->wParam);
        }
        if (IsBlockedWhileMenuOpen(msg->message))
        {
            if (msg->message == WM_INPUT)
                DefWindowProcW(msg->hwnd, msg->message, msg->wParam, msg->lParam);  // Frees the raw input buffer
            msg->message = WM_NULL;
        }

        return ret;
    }

    BOOL WINAPI HookedPeekMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
    {
        return FilterPeeked(oPeekMessageW(msg, hwnd, min, max, remove), msg, remove);
    }

    BOOL WINAPI HookedPeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
    {
        return FilterPeeked(oPeekMessageA(msg, hwnd, min, max, remove), msg, remove);
    }

    LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        // Messages that did not come through PeekMessage (or arrive while the menu is closed) end up here.
        // Fresh key presses only (bit 30 = auto-repeat)
        if (msg == WM_KEYDOWN && !(lp & (1 << 30)))
        {
            if (OnKeyDown(wp)) return 0;
        }

        if (gui::visible)
        {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            const ImGuiIO& io = ImGui::GetIO();
            // Swallow input the menu wants so it doesn't reach the game.
            if (io.WantCaptureMouse &&
                ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT))
                return 0;
            if (io.WantCaptureKeyboard &&
                (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                 msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP))
                return 0;
        }
        return CallWindowProcW(oWndProc, hwnd, msg, wp, lp);
    }

    void InitImGui(IDirect3DDevice9* dev)
    {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        const HRESULT cpHr = dev->GetCreationParameters(&cp);
        g_hwnd = cp.hFocusWindow;
#if HOOKS_DIAG
        HOOKS_LOG(info, "ImGui init: device={} focus window={} adapter={} type={} flags=0x{:X} (hr=0x{:08X})",
                  static_cast<void*>(dev), static_cast<void*>(cp.hFocusWindow), cp.AdapterOrdinal,
                  static_cast<int>(cp.DeviceType), cp.BehaviorFlags, static_cast<unsigned long>(cpHr));
        LogWindow("Device window", g_hwnd);
#else
        (void)cpHr;
#endif
        if (!g_hwnd) HOOKS_LOG(error, "The game's device has no focus window: the menu will get no input");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        static const std::string iniPath = (paths::Dir() / "MedgeMenu.ini").string();
        ImGui::GetIO().IniFilename = iniPath.c_str(); // Remembers window positions
        gui::ApplyStyle();
        if (!ImGui_ImplWin32_Init(g_hwnd)) HOOKS_LOG(error, "ImGui_ImplWin32_Init failed");
        if (!ImGui_ImplDX9_Init(dev)) HOOKS_LOG(error, "ImGui_ImplDX9_Init failed");

        SetLastError(0);
        oWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        if (!oWndProc)
            HOOKS_LOG(error, "Subclassing the game window failed (error {}): the menu gets no input", GetLastError());
        g_imguiReady = true;
        HOOKS_LOG(info, "ImGui initialised (hwnd={})", static_cast<void*>(g_hwnd));
    }

    HRESULT WINAPI HookedEndScene(IDirect3DDevice9* dev)
    {
        if (g_endSceneCalls.fetch_add(1, std::memory_order_relaxed) == 0)
            HOOKS_LOG(info, "First EndScene call (device={}, thread {})", static_cast<void*>(dev), GetCurrentThreadId());
        if (!g_imguiReady) InitImGui(dev);

        // modules::Tick() runs on the game thread now (see game::OnTick in dllmain.cpp)

#if MEMENU_GAME
        debugrender::BeginFrame(dev);   // Matrices for this frame (labels use them too)
        debugrender::Render(dev);       // Overlay path, when the in-scene pass did not draw
#endif

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = gui::visible;
#if MEMENU_GAME
        game::SetInputBlocked(gui::visible);        // Also ignore look input (gamepad) while the menu is open
#endif
        DrainRenderQueue();
        gui::Render();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
#if MEMENU_GAME
        debugrender::EndFrame();
#endif

        return oEndScene(dev);
    }

    HRESULT WINAPI HookedReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
    {
#if MEMENU_GAME
        debugrender::OnLostDevice();
#endif
        if (g_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
        HRESULT hr = oReset(dev, pp);
#if HOOKS_DIAG
        if (pp)
            HOOKS_LOG(info, "Device Reset to {}x{} {}: hr=0x{:08X} {}", pp->BackBufferWidth, pp->BackBufferHeight,
                      pp->Windowed ? "windowed" : "exclusive fullscreen", static_cast<unsigned long>(hr), HResultName(hr));
#endif
        if (SUCCEEDED(hr) && g_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
        return hr;
    }

#if MEMENU_GAME
    // The engine resolves its scene color here, with the world's depth buffer still bound: the debug renderer draws
    // into the frame at that point so the world hides what is behind it.
    HRESULT WINAPI HookedStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* srcRect,
                                     IDirect3DSurface9* dst, const RECT* dstRect, D3DTEXTUREFILTERTYPE filter)
    {
        debugrender::OnStretchRect(dev, src, dst);
        return oStretchRect(dev, src, srcRect, dst, dstRect, filter);
    }
#endif

    // Create a throwaway device just to read the IDirect3DDevice9 vtable. Returns the HRESULT of the attempt
    // (E_FAIL when Direct3DCreate9 itself failed).
    //
    // D3DDEVTYPE_NULLREF is the fallback: a HAL device can't be created while the game holds exclusive fullscreen
    // (D3DERR_DEVICELOST for as long as it runs, which is every time the menu is loaded after the game went
    // fullscreen), but a NULLREF device never touches the adapter. d3d9.dll uses one device class for every type, so
    // its vtable is the very one the game's HAL device uses (checked: all 119 entries are the same).
    HRESULT TryGetVTable(void** vtable, size_t count, bool verbose, D3DDEVTYPE type)
    {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MEMenuDummy";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            HOOKS_LOG(warn, "RegisterClassExW for the dummy window failed (error {})", GetLastError());
        HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd) HOOKS_LOG(error, "Creating the dummy window failed (error {})", GetLastError());

        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d) HOOKS_LOG(error, "Direct3DCreate9(D3D_SDK_VERSION={}) returned null", D3D_SDK_VERSION);
#if HOOKS_DIAG
        if (verbose) LogD3D9Environment(d3d);
#else
        (void)verbose;
#endif
        HRESULT hr = E_FAIL;
        if (d3d)
        {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = wnd;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            IDirect3DDevice9* dev = nullptr;
            hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, type, wnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
            if (SUCCEEDED(hr))
            {
                memcpy(vtable, *reinterpret_cast<void***>(dev), count * sizeof(void*));
                dev->Release();
            }
            d3d->Release();
        }
        DestroyWindow(wnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return hr;
    }

    // The dummy device can fail while the game is busy with its own device (startup, exclusive fullscreen, a lost
    // device after alt-tab): fall back to a NULLREF device at once, and try both again for a while before giving up.
    bool GetVTable(void** vtable, size_t count)
    {
        constexpr int kAttempts = 20;
        constexpr DWORD kDelayMs = 500;
        for (int attempt = 1; attempt <= kAttempts; ++attempt)
        {
            const HRESULT hal = TryGetVTable(vtable, count, attempt == 1, D3DDEVTYPE_HAL);
            if (SUCCEEDED(hal))
            {
                if (attempt > 1) HOOKS_LOG(info, "Read the D3D9 vtable on attempt {}", attempt);
                return true;
            }
            const HRESULT nullref = TryGetVTable(vtable, count, false, D3DDEVTYPE_NULLREF);
            if (SUCCEEDED(nullref))
            {
                HOOKS_LOG(info, "Read the D3D9 vtable from a NULLREF device (a HAL device failed: hr=0x{:08X} {}; "
                          "the game has exclusive fullscreen)", static_cast<unsigned long>(hal), HResultName(hal));
                return true;
            }
            if (attempt == 1 || attempt == kAttempts)
            {
                HOOKS_LOG(warn, "Creating the dummy D3D9 device failed: HAL hr=0x{:08X} {}, NULLREF hr=0x{:08X} {}, "
                          "attempt {}/{}", static_cast<unsigned long>(hal), HResultName(hal),
                          static_cast<unsigned long>(nullref), HResultName(nullref), attempt, kAttempts);
#if HOOKS_DIAG
                LogGameWindow();   // Minimized or exclusive fullscreen are the usual reasons
#endif
            }
            if (attempt < kAttempts) Sleep(kDelayMs);
        }
        return false;
    }
}

bool hooks::Install()
{
    HOOKS_LOG(info, "Installing hooks (thread {})", GetCurrentThreadId());
#if HOOKS_DIAG
    LogGameWindow();
#endif
    void* vt[119]{}; // IDirect3DDevice9 has 119 methods
    if (!GetVTable(vt, 119))
    {
        HOOKS_LOG(error, "Failed to read the D3D9 vtable");
        return false;
    }

    constexpr int kReset = 16, kEndScene = 42;
#if MEMENU_GAME
    constexpr int kStretchRect = 34;
#endif
#if HOOKS_DIAG
    // Where the methods we hook live, and whether something else hooked them first
    HOOKS_LOG(info, "EndScene: {} [{}]", DescribeAddress(vt[kEndScene]), DescribePrologue(vt[kEndScene]));
    HOOKS_LOG(info, "Reset: {} [{}]", DescribeAddress(vt[kReset]), DescribePrologue(vt[kReset]));
#if MEMENU_GAME
    HOOKS_LOG(info, "StretchRect: {} [{}]", DescribeAddress(vt[kStretchRect]), DescribePrologue(vt[kStretchRect]));
#endif
#endif

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK)
    {
        HOOKS_LOG(error, "MH_Initialize failed: {}", MH_StatusToString(st));
        return false;
    }

    if ((st = MH_CreateHook(vt[kEndScene], reinterpret_cast<void*>(&HookedEndScene), reinterpret_cast<void**>(&oEndScene))) != MH_OK)
    { HOOKS_LOG(error, "Hooking EndScene failed: {}", MH_StatusToString(st)); return false; }
    if ((st = MH_CreateHook(vt[kReset], reinterpret_cast<void*>(&HookedReset), reinterpret_cast<void**>(&oReset))) != MH_OK)
    { HOOKS_LOG(error, "Hooking Reset failed: {}", MH_StatusToString(st)); return false; }

#if MEMENU_GAME
    // Optional: only the collision view's in-scene drawing needs it (it falls back to an overlay).
    if ((st = MH_CreateHook(vt[kStretchRect], reinterpret_cast<void*>(&HookedStretchRect), reinterpret_cast<void**>(&oStretchRect))) != MH_OK)
        HOOKS_LOG(warn, "Hooking StretchRect failed ({}): the collision view will draw as an overlay", MH_StatusToString(st));
#endif

    // Optional: without these the menu still works, but the game keeps reading the mouse while it is open.
    if ((st = MH_CreateHookApi(L"user32", "PeekMessageW", reinterpret_cast<void*>(&HookedPeekMessageW), reinterpret_cast<void**>(&oPeekMessageW))) != MH_OK)
        HOOKS_LOG(warn, "Hooking PeekMessageW failed ({}): the game will still see mouse input while the menu is open", MH_StatusToString(st));
    if ((st = MH_CreateHookApi(L"user32", "PeekMessageA", reinterpret_cast<void*>(&HookedPeekMessageA), reinterpret_cast<void**>(&oPeekMessageA))) != MH_OK)
        HOOKS_LOG(warn, "Hooking PeekMessageA failed ({})", MH_StatusToString(st));

    if ((st = MH_EnableHook(MH_ALL_HOOKS)) != MH_OK) { HOOKS_LOG(error, "MH_EnableHook failed: {}", MH_StatusToString(st)); return false; }

    HOOKS_LOG(info, "Hooks installed (EndScene={}, Reset={})", vt[kEndScene], vt[kReset]);
    return true;
}

unsigned hooks::EndSceneCalls() { return g_endSceneCalls.load(std::memory_order_relaxed); }

void hooks::RunOnRenderThread(std::function<void()> fn)
{
    std::lock_guard lock(g_renderQueueMutex);
    g_renderQueue.push_back(std::move(fn));
}

void hooks::Uninstall()
{
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(250);     // Let in-flight hooked calls drain
    {
        std::lock_guard lock(g_renderQueueMutex);
        g_renderQueue.clear();  // Never runs now: the closures may point into modules that are about to go away
    }
#if MEMENU_GAME
    debugrender::Shutdown();
#endif
    if (g_imguiReady)
    {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    MH_Uninitialize();
    HOOKS_LOG(info, "Hooks removed");
}
