// medge_launcher: start (or wait for) Mirror's Edge, then load medge_menu.dll into it.
//
//  medge_launcher                      Inject into the running game, or find the install and start it
//  medge_launcher --no-start           Only wait for MirrorsEdge.exe (start the game yourself)
//  medge_launcher --game "D:\...\MirrorsEdge.exe"
//  medge_launcher --steam              Start it through Steam (app id 17410) and wait
//  medge_launcher --pid 1234           Inject into a specific process right now
// See --help for everything. Defaults can be stored in launcher.json next to the launcher.

#include "detect.h"
#include "inject.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
    constexpr unsigned kDefaultSteamAppId = detect::kSteamAppId;  // Mirror's Edge on Steam

    struct Options
    {
        std::wstring process = L"MirrorsEdge.exe";
        fs::path dll;                               // default: medge_menu.dll next to the launcher
        fs::path game;                              // Executable to start (optional)
        std::optional<unsigned> steamAppId;         // Start through Steam instead (optional)
        DWORD pid = 0;                              // Inject into this process, skip launching/waiting
        int delayMs = 2000;                         // Extra wait after the game window shows up
        int timeoutSec = 120;                       // How long to wait for the process/window
        bool autoStart = true;                      // Not running and no --game/--steam: find the install, start it
        bool save = false;                          // Write the effective options to launcher.json
    };

    // --- Small Helpers ---
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    fs::path ExeDir()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path();
    }

    void Info(const wchar_t* fmt, const wchar_t* a = L"", const wchar_t* b = L"")
    {
        std::wprintf(fmt, a, b);
        std::wprintf(L"\n");
    }

    // ---- launcher.json ---------------------------------------------------------------
    // { "process": "MirrorsEdge.exe", "dll": "medge_menu.dll", "game": "D:/.../MirrorsEdge.exe",
    //   "steam_app_id": 17410, "delay_ms": 2000, "timeout_s": 120, "auto_start": true }   (all keys optional)
    void LoadConfig(Options& o, const fs::path& file, const fs::path& base)
    {
        std::ifstream in(file);
        if (!in) return;
        try
        {
            const json j = json::parse(in, nullptr, true, /*ignore_comments*/ true);
            const auto path = [&](const char* key, fs::path& out)
            {
                if (j.contains(key) && j[key].is_string() && !j[key].get<std::string>().empty())
                {
                    fs::path p(Widen(j[key].get<std::string>()));
                    out = p.is_absolute() ? p : base / p;   // Relative paths in the file are relative to the launcher
                }
            };
            if (j.contains("process") && j["process"].is_string()) o.process = Widen(j["process"].get<std::string>());
            path("dll", o.dll);
            path("game", o.game);
            if (j.contains("steam_app_id") && j["steam_app_id"].is_number_unsigned()) o.steamAppId = j["steam_app_id"].get<unsigned>();
            if (j.contains("delay_ms")  && j["delay_ms"].is_number_integer())  o.delayMs    = j["delay_ms"].get<int>();
            if (j.contains("timeout_s") && j["timeout_s"].is_number_integer()) o.timeoutSec = j["timeout_s"].get<int>();
            if (j.contains("auto_start") && j["auto_start"].is_boolean())      o.autoStart  = j["auto_start"].get<bool>();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "Ignoring %s: %s\n", file.string().c_str(), e.what());
        }
    }


    bool SaveConfig(const Options& o, const fs::path& file)
    {
        json j = json::object();
        j["process"] = Narrow(o.process);
        j["dll"] = Narrow(o.dll.wstring());
        if (!o.game.empty()) j["game"] = Narrow(o.game.wstring());
        if (o.steamAppId)    j["steam_app_id"] = *o.steamAppId;
        j["delay_ms"] = o.delayMs;
        j["timeout_s"] = o.timeoutSec;
        j["auto_start"] = o.autoStart;
        std::ofstream out(file);
        if (!out) return false;
        out << j.dump(2) << '\n';
        return out.good();
    }

    // Command Line

    void PrintUsage()
    {
        std::wprintf(
            L"medge_launcher - load medge_menu.dll into Mirror's Edge\n\n"
            L"  --game <exe>      start this executable (working directory = its folder)\n"
            L"  --steam [appid]   start through Steam (default app id %u) and wait for the process\n"
            L"  --pid <n>         inject into this process id right away\n"
            L"  --no-start        don't start the game, only wait for it\n"
            L"  --list            show the Mirror's Edge installs that were found and exit\n"
            L"  --dll <path>      DLL to inject (default: medge_menu.dll next to the launcher)\n"
            L"  --process <name>  process to wait for (default: MirrorsEdge.exe)\n"
            L"  --delay <ms>      extra wait after the game window appears (default 2000)\n"
            L"  --timeout <s>     how long to wait for the game (default 120)\n"
            L"  --save            store these options in launcher.json next to the launcher\n"
            L"  --help            this text\n\n"
            L"If the game is already running it is used as is. Otherwise, with no --game/--steam, the launcher\n"
            L"looks for the install (Steam libraries, EA/Origin registry, installed programs) and starts it.\n",
            kDefaultSteamAppId);
    }

    // Returns -1 to continue, otherwise the exit code.
    int ParseArgs(int argc, wchar_t** argv, Options& o)
    {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            const auto value = [&](const wchar_t* name) -> const wchar_t* {
                if (i + 1 >= argc) { std::fwprintf(stderr, L"%ls needs a value\n", name); return nullptr; }
                return argv[++i];
            };

            if (a == L"--help" || a == L"-h" || a == L"/?") { PrintUsage(); return 0; }
            else if (a == L"--save") o.save = true;
            else if (a == L"--no-start") o.autoStart = false;
            else if (a == L"--list") {
                const auto installs = detect::FindInstalls(o.process);
                if (installs.empty()) std::wprintf(L"No Mirror's Edge install found.\n");
                for (const auto& in : installs)
                    std::wprintf(L"%ls\n    found in %ls%ls\n", in.exe.c_str(), in.source.c_str(),
                                 in.steamAppId ? L" (started through Steam)" : L"");
                return 0;
            }
            else if (a == L"--steam") {
                o.steamAppId = kDefaultSteamAppId;
                if (i + 1 < argc && argv[i + 1][0] != L'-') o.steamAppId = static_cast<unsigned>(_wtoi(argv[++i]));
            }
            else if (a == L"--game")    { const auto* v = value(L"--game");    if (!v) return 1; o.game = fs::absolute(v); }
            else if (a == L"--dll")     { const auto* v = value(L"--dll");     if (!v) return 1; o.dll = fs::absolute(v); }
            else if (a == L"--process") { const auto* v = value(L"--process"); if (!v) return 1; o.process = v; }
            else if (a == L"--pid")     { const auto* v = value(L"--pid");     if (!v) return 1; o.pid = static_cast<DWORD>(_wtoi(v)); }
            else if (a == L"--delay")   { const auto* v = value(L"--delay");   if (!v) return 1; o.delayMs = _wtoi(v); }
            else if (a == L"--timeout") { const auto* v = value(L"--timeout"); if (!v) return 1; o.timeoutSec = _wtoi(v); }
            else { std::fwprintf(stderr, L"Unknown option: %ls (see --help)\n", a.c_str()); return 1; }
        }
        return -1;
    }

    // Waiting for the game
    struct WindowSearch { DWORD pid; bool found; };

    BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
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

    bool HasWindow(DWORD pid)
    {
        WindowSearch s{pid, false};
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&s));
        return s.found;
    }

    // Waits until a process with this name has a visible main window and returns it. Every process with the name is
    // looked at on each pass: a Steam copy started directly restarts itself through Steam (new pid), and the first
    // process may exit before it ever shows a window. At the timeout a process without a window is still returned.
    std::optional<DWORD> WaitForGame(const std::wstring& process, DWORD timeoutMs, bool& waitedForWindow)
    {
        const DWORD start = GetTickCount();
        bool announced = false;
        waitedForWindow = false;
        do {
            const auto pids = inject::FindProcesses(process);
            for (DWORD pid : pids)
                if (HasWindow(pid)) return pid;
            if (!pids.empty()) {
                waitedForWindow = true;
                if (!announced) { Info(L"Found %ls, waiting for its window...", process.c_str()); announced = true; }
            }
            Sleep(250);
        } while (GetTickCount() - start < timeoutMs);
        if (const auto pid = inject::FindProcess(process)) {
            Info(L"No window yet, injecting anyway.");
            return pid;
        }
        return std::nullopt;
    }

    // Not running and nothing to start given: start the install that was found (if allowed).
    void PickInstall(Options& o)
    {
        if (!o.game.empty() || o.steamAppId || !o.autoStart) return;
        const auto installs = detect::FindInstalls(o.process);
        if (installs.empty()) {
            Info(L"No Mirror's Edge install found (use --game <exe> or --steam).");
            return;
        }
        const detect::Install& best = installs.front();
        Info(L"Found the game: %ls", best.exe.c_str());
        Info(L"    (%ls)", best.source.c_str());
        // Steam copies are started through Steam: started directly they restart themselves through it anyway.
        if (best.steamAppId) o.steamAppId = best.steamAppId;
        else o.game = best.exe;
    }

    bool StartGame(const Options& o)
    {
        if (!o.game.empty()) {
            std::wstring cmd = L"\"" + o.game.wstring() + L"\"";
            const std::wstring dir = o.game.parent_path().wstring();
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(o.game.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
                std::fwprintf(stderr, L"Could not start %ls (error %lu)\n", o.game.c_str(), GetLastError());
                return false;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Info(L"Started %ls", o.game.c_str());
            return true;
        }
        if (o.steamAppId) {
            const std::wstring url = L"steam://rungameid/" + std::to_wstring(*o.steamAppId);
            const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (rc <= 32) { std::fwprintf(stderr, L"Could not open %ls (is Steam installed?)\n", url.c_str()); return false; }
            Info(L"Asked Steam to start the game (%ls)", url.c_str());
            return true;
        }
        return true;   // nothing to start: wait for the user to start it
    }

    // Keep a double-clicked console window open so the messages can be read.
    bool OwnsConsole()
    {
        DWORD list[2];
        return GetConsoleProcessList(list, 2) <= 1;
    }

    int Run(Options& o)
    {
        // --- DLL sanity checks ---------------------------------------------------------------
        if (!fs::exists(o.dll)) {
            std::fwprintf(stderr, L"DLL not found: %ls\n", o.dll.c_str());
            return 1;
        }
        const auto machine = inject::PeMachine(o.dll);
        if (!machine) { std::fwprintf(stderr, L"%ls is not a valid PE file\n", o.dll.c_str()); return 1; }
        if (*machine != IMAGE_FILE_MACHINE_I386) {
            std::fwprintf(stderr, L"%ls is not a 32-bit DLL (Mirror's Edge is 32-bit)\n", o.dll.c_str());
            return 1;
        }

        // --- find or start the game ------------------------------------------------------------
        DWORD pid = o.pid;
        if (!pid) {
            const DWORD timeout = static_cast<DWORD>(std::max(1, o.timeoutSec)) * 1000;
            const bool running = !inject::FindProcesses(o.process).empty();
            if (running) {
                Info(L"%ls is already running", o.process.c_str());
            } else {
                PickInstall(o);
                if (!StartGame(o)) return 2;
                if (o.game.empty() && !o.steamAppId) Info(L"Waiting for %ls - start the game now...", o.process.c_str());
            }

            bool waited = false;
            const auto found = WaitForGame(o.process, timeout, waited);
            if (!found) {
                std::fwprintf(stderr, L"Timed out waiting for %ls\n", o.process.c_str());
                return 2;
            }
            pid = *found;
            // A game that already had its window is up and running: only a freshly started one needs time to settle.
            if ((!running || waited) && o.delayMs > 0) Sleep(static_cast<DWORD>(o.delayMs));
        }

        // --- inject --------------------------------------------------------------------------------
        const std::wstring dllName = o.dll.filename().wstring();
        if (inject::IsModuleLoaded(pid, dllName)) {
            Info(L"%ls is already loaded in the game, nothing to do.", dllName.c_str());
            return 0;
        }

        const inject::Result r = inject::LoadLibraryInto(pid, o.dll);
        if (!r.ok) {
            std::fwprintf(stderr, L"Injection failed: %ls\n", r.message.c_str());
            return 3;
        }
        Info(L"%ls", r.message.c_str());
        Info(L"Press INSERT in the game to open the menu, END to unload it.");
        return 0;
    }

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options opt;
    const fs::path dir = ExeDir();
    LoadConfig(opt, dir / L"launcher.json", dir);
    if (const int rc = ParseArgs(argc, argv, opt); rc >= 0) return rc;
    if (opt.dll.empty()) opt.dll = dir / L"medge_menu.dll";

    if (opt.save) {
        if (SaveConfig(opt, dir / L"launcher.json")) Info(L"Saved %ls", (dir / L"launcher.json").c_str());
        else std::fwprintf(stderr, L"Could not write launcher.json\n");
    }

    const bool pause = OwnsConsole();
    const int rc = Run(opt);
    if (pause) {
        std::wprintf(L"\nPress Enter to close...");
        std::getchar();
    }
    return rc;

}