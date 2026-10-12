#include "hooks.h"
#include "gui.h"
#include "modules.h"
#include "paths.h"

#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include "game/game.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
    using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    EndScene_t oEndScene = nullptr;
    Reset_t oReset = nullptr;
    WNDPROC oWndProc = nullptr;
    HWND g_hwnd = nullptr;
    bool g_imguiReady = false;

    using PeekMessageW_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    using PeekMessageA_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    PeekMessageW_t oPeekMessageW = nullptr;
    PeekMessageA_t oPeekMessageA = nullptr;

    // Fresh key press: "press a key to bind", the menu toggle, module hotkeys. True if the key was consumed.
    bool OnKeyDown(WPARAM wp)
    {
        if (gui::CaptureKey(static_cast<int>(wp))) return true;
        if (wp == VK_INSERT) gui::visible = !gui::visible;
        else modules::OnKey(static_cast<int>(wp));
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
        dev->GetCreationParameters(&cp);
        g_hwnd = cp.hFocusWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        static const std::string iniPath = (paths::Dir() / "MedgeMenu.ini").string();
        ImGui::GetIO().IniFilename = iniPath.c_str(); // Remembers window positions
        gui::ApplyStyle();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX9_Init(dev);

        oWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        g_imguiReady = true;
        spdlog::info("ImGui initialised (hwnd={})", static_cast<void*>(g_hwnd));
    }

    HRESULT WINAPI HookedEndScene(IDirect3DDevice9* dev)
    {
        if (!g_imguiReady) InitImGui(dev);

        // modules::Tick() runs on the game thread now (see game::OnTick in dllmain.cpp)

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = gui::visible;
        game::SetInputBlocked(gui::visible);        // Also ignore look input (gamepad) while the menu is open
        gui::Render();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

        return oEndScene(dev);
    }

    HRESULT WINAPI HookedReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
    {
        if (g_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
        HRESULT hr = oReset(dev, pp);
        if (SUCCEEDED(hr) && g_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
        return hr;
    }

    // Create a throwaway device just to read the IDirect3DDevice9 vtable.
    bool GetVTable(void** vtable, size_t count)
    {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MEMenuDummy";
        RegisterClassExW(&wc);
        HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        bool ok = false;
        if (d3d)
        {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = wnd;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            IDirect3DDevice9* dev = nullptr;
            if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                &pp, &dev)))
            {
                memcpy(vtable, *reinterpret_cast<void***>(dev), count * sizeof(void*));
                dev->Release();
                ok = true;
            }
            d3d->Release();
        }
        DestroyWindow(wnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }
}

bool hooks::Install()
{
    void* vt[119]{}; // IDirect3DDevice9 has 119 methods
    if (!GetVTable(vt, 119))
    {
        spdlog::error("Failed to read the D3D9 vtable");
        return false;
    }
    if (MH_Initialize() != MH_OK)
    {
        spdlog::error("MH_Initialize failed");
        return false;
    }

    constexpr int kReset = 16, kEndScene = 42;
    if (MH_CreateHook(vt[kEndScene], &HookedEndScene, reinterpret_cast<void**>(&oEndScene)) != MH_OK) { spdlog::error("Hooking EndScene failed"); return false; }
    if (MH_CreateHook(vt[kReset], &HookedReset, reinterpret_cast<void**>(&oReset)) != MH_OK) { spdlog::error("Hooking Reset failed"); return false; }

    // Optional: without these the menu still works, but the game keeps reading the mouse while it is open.
    if (MH_CreateHookApi(L"user32", "PeekMessageW", reinterpret_cast<void*>(&HookedPeekMessageW), reinterpret_cast<void**>(&oPeekMessageW)) != MH_OK)
        spdlog::warn("Hooking PeekMessageW failed: the game will still see mouse input while the menu is open");
    if (MH_CreateHookApi(L"user32", "PeekMessageA", reinterpret_cast<void*>(&HookedPeekMessageA), reinterpret_cast<void**>(&oPeekMessageA)) != MH_OK)
        spdlog::warn("Hooking PeekMessageA failed");

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { spdlog::error("MH_EnableHook failed"); return false; }

    spdlog::info("Hooks installed (EndScene={}, Reset={})", vt[kEndScene], vt[kReset]);
    return true;
}

void hooks::Uninstall()
{
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(250);     // Let in-flight hooked calls drain
    if (g_imguiReady)
    {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    MH_Uninitialize();
    spdlog::info("Hooks removed");
}
