// memenu_installer: one window, one click. Finds Mirror's Edge, puts the loader (dinput8.dll) and the menu into its
// Binaries folder, and takes them out again. From a script: memenu_installer --install|--uninstall [game folder] [--quiet]
#include "install.h"
#include "resource.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t kTitle[] = L"Mirror's Edge Menu Installer";

    enum : int { kIdPath = 1001, kIdBrowse, kIdInstall, kIdUninstall, kIdClose, kIdStatus };

    HWND g_wnd, g_path, g_status, g_install, g_uninstall;
    HFONT g_font, g_titleFont;
    int g_dpi = 96;

    int S(int px) { return MulDiv(px, g_dpi, 96); }

    std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    fs::path CurrentDir()
    {
        wchar_t buf[MAX_PATH * 2]{};
        GetWindowTextW(g_path, buf, static_cast<int>(std::size(buf)));
        return buf;
    }

    // Status line and button states for the folder in the edit box.
    void Refresh()
    {
        const auto dir = install::NormalizeGameDir(CurrentDir());
        std::wstring text;
        bool canInstall = false, canUninstall = false;
        std::wstring installLabel = L"Install";

        if (!dir) {
            text = L"Mirror's Edge was not found here. Click Browse and pick MirrorsEdge.exe "
                   L"(in the game's Binaries folder).";
        } else {
            const install::Status s = install::Inspect(*dir);
            canInstall = true;
            if (s.installed) {
                const std::wstring v = Widen(s.installedVersion);
                text = L"Installed (version " + v + L").";
                installLabel = s.installedVersion == install::Version() ? L"Reinstall" : L"Update";
                canUninstall = true;
            } else {
                text = L"Ready to install.";
                if (s.foreignLoader)
                    text += L" Another mod's dinput8.dll is there: it will be kept as a backup and put back on uninstall.";
            }
            if (s.gameRunning) text += L"\nMirror's Edge is running: close it before installing or uninstalling.";
        }
        SetWindowTextW(g_status, text.c_str());
        SetWindowTextW(g_install, installLabel.c_str());
        EnableWindow(g_install, canInstall);
        EnableWindow(g_uninstall, canUninstall);
    }

    void Browse()
    {
        wchar_t file[MAX_PATH]{};
        const fs::path start = CurrentDir();
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = g_wnd;
        ofn.lpstrFilter = L"Mirror's Edge (MirrorsEdge.exe)\0MirrorsEdge.exe\0Programs (*.exe)\0*.exe\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
        ofn.lpstrTitle = L"Pick MirrorsEdge.exe";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;

        const auto dir = install::NormalizeGameDir(file);
        SetWindowTextW(g_path, dir ? dir->c_str() : fs::path(file).parent_path().c_str());
        Refresh();
    }

    void DoInstall()
    {
        const auto dir = install::NormalizeGameDir(CurrentDir());
        if (!dir) return;
        std::wstring msg;
        const bool ok = install::Install(*dir, msg);
        MessageBoxW(g_wnd, msg.c_str(), kTitle, ok ? MB_ICONINFORMATION : MB_ICONERROR);
        Refresh();
    }

    void DoUninstall()
    {
        const auto dir = install::NormalizeGameDir(CurrentDir());
        if (!dir) return;
        bool removeSettings = false;
        if (install::Inspect(*dir).hasSettings) {
            const int answer = MessageBoxW(g_wnd,
                L"Also delete your menu settings (config, themes, logs)?\n\n"
                L"Yes: delete everything.\nNo: keep them for the next install.",
                kTitle, MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (answer == IDCANCEL) return;
            removeSettings = answer == IDYES;
        }
        std::wstring msg;
        const bool ok = install::Uninstall(*dir, removeSettings, msg);
        MessageBoxW(g_wnd, msg.c_str(), kTitle, ok ? MB_ICONINFORMATION : MB_ICONERROR);
        Refresh();
    }

    HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font,
                 DWORD exStyle = 0)
    {
        HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }

    void CreateControls()
    {
        const std::wstring version = L"Version " + Widen(install::Version()) +
                                     L"  •  installs into the game folder, nothing else on your PC is changed";
        Control(L"STATIC", L"Mirror's Edge Menu", 0, 16, 12, 488, 28, 0, g_titleFont);
        Control(L"STATIC", version.c_str(), 0, 16, 42, 488, 18, 0, g_font);

        Control(L"STATIC", L"Game folder:", 0, 16, 74, 488, 18, 0, g_font);
        g_path = Control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 16, 94, 400, 24, kIdPath, g_font, WS_EX_CLIENTEDGE);
        Control(L"BUTTON", L"Browse...", WS_TABSTOP, 424, 93, 80, 26, kIdBrowse, g_font);

        g_status = Control(L"STATIC", L"", 0, 16, 130, 488, 56, kIdStatus, g_font);

        g_uninstall = Control(L"BUTTON", L"Uninstall", WS_TABSTOP, 16, 198, 100, 30, kIdUninstall, g_font);
        g_install = Control(L"BUTTON", L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, 296, 198, 104, 30, kIdInstall, g_font);
        Control(L"BUTTON", L"Close", WS_TABSTOP, 408, 198, 96, 30, kIdClose, g_font);

        if (const auto dir = install::DetectGameDir()) SetWindowTextW(g_path, dir->c_str());
        Refresh();
        SetFocus(g_install);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
            case WM_CREATE:
                g_wnd = hwnd;
                CreateControls();
                SetTimer(hwnd, 1, 2000, nullptr);   // Notice the game being started/closed
                return 0;
            case WM_TIMER:
                Refresh();
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case kIdBrowse: Browse(); return 0;
                    case kIdInstall: DoInstall(); return 0;
                    case kIdUninstall: DoUninstall(); return 0;
                    case kIdClose: DestroyWindow(hwnd); return 0;
                    case kIdPath: if (HIWORD(wp) == EN_CHANGE) Refresh(); return 0;
                }
                break;
            case WM_CTLCOLORSTATIC:
                SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CreateFonts()
    {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        g_font = CreateFontIndirectW(&lf);
        lf.lfHeight = -MulDiv(15, g_dpi, 72);
        lf.lfWeight = FW_SEMIBOLD;
        g_titleFont = CreateFontIndirectW(&lf);
    }

    int RunGui(HINSTANCE inst, int show)
    {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        CreateFonts();

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"MemenuInstaller";
        RegisterClassExW(&wc);

        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT r{0, 0, S(520), S(244)};
        AdjustWindowRectEx(&r, style, FALSE, 0);
        const int w = r.right - r.left, h = r.bottom - r.top;
        HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, kTitle, style,
                                    (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                                    nullptr, nullptr, inst, nullptr);
        ShowWindow(hwnd, show);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(hwnd, &msg)) continue;   // Tab / Enter / Esc between the controls
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }

    // memenu_installer --install|--uninstall [dir] [--quiet] [--remove-settings]
    // Exit codes: 0 done, 1 failed, 2 game not found, 3 game is running (install.ps1 tells the player what to do).
    int RunCli(int argc, wchar_t** argv)
    {
        bool doInstall = false, doUninstall = false, quiet = false, removeSettings = false;
        fs::path dir;
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--install") doInstall = true;
            else if (a == L"--uninstall") doUninstall = true;
            else if (a == L"--quiet") quiet = true;
            else if (a == L"--remove-settings") removeSettings = true;
            else dir = a;
        }
        const auto report = [&](int code, const std::wstring& text) {
            if (!quiet) MessageBoxW(nullptr, text.c_str(), kTitle, code == 0 ? MB_ICONINFORMATION : MB_ICONERROR);
            return code;
        };

        std::optional<fs::path> gameDir = dir.empty() ? install::DetectGameDir() : install::NormalizeGameDir(dir);
        if (!gameDir) return report(2, L"Mirror's Edge was not found. Pass the game folder on the command line.");
        if (install::Inspect(*gameDir).gameRunning)
            return report(3, L"Mirror's Edge is running. Close the game, then try again.");

        std::wstring msg;
        const bool ok = doInstall ? install::Install(*gameDir, msg) : install::Uninstall(*gameDir, removeSettings, msg);
        return report(ok ? 0 : 1, msg);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = -1;
    for (int i = 1; argv && i < argc; ++i)
        if (!std::wcscmp(argv[i], L"--install") || !std::wcscmp(argv[i], L"--uninstall")) { rc = RunCli(argc, argv); break; }
    if (argv) LocalFree(argv);
    return rc >= 0 ? rc : RunGui(inst, show);
}
