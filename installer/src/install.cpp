#include "install.h"
#include "resource.h"

#include "detect.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>

#include <cwctype>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace install
{
    namespace
    {
        constexpr const wchar_t* kGameExe = L"MirrorsEdge.exe";
        constexpr const wchar_t* kLoader = L"dinput8.dll";
        constexpr const wchar_t* kBackup = L"dinput8.dll.memenu-backup";
        constexpr const wchar_t* kMenuDir = L"memenu";
        constexpr const wchar_t* kMenuDll = L"medge_menu.dll";
        constexpr char kMarker[] = "memenu-loader:";
        constexpr const wchar_t* kUninstaller = L"uninstall.exe";
        bool g_registerApp = true;      // SetAppRegistration
        constexpr const wchar_t* kAppKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MirrorsEdgeMenu";

        std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

        std::wstring ErrorText(DWORD code)
        {
            wchar_t buf[256]{};
            const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                           buf, static_cast<DWORD>(std::size(buf)), nullptr);
            std::wstring s(buf, n);
            while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
            return s;
        }

        bool GameRunning()
        {
            const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return false;
            PROCESSENTRY32W pe{sizeof(pe)};
            bool found = false;
            for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
                found = _wcsicmp(pe.szExeFile, kGameExe) == 0;
            CloseHandle(snap);
            return found;
        }

        // Version string after the loader's marker, or nothing if the file isn't our loader.
        std::optional<std::string> LoaderVersion(const fs::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) return std::nullopt;
            const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto pos = data.find(kMarker);
            if (pos == std::string::npos) return std::nullopt;
            const auto start = pos + sizeof(kMarker) - 1;
            const auto end = data.find('\0', start);
            return data.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }

        struct Payload { const void* data = nullptr; DWORD size = 0; };

        Payload Resource(int id)
        {
            HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
            if (!res) return {};
            HGLOBAL h = LoadResource(nullptr, res);
            if (!h) return {};
            return {LockResource(h), SizeofResource(nullptr, res)};
        }

        // Written next to the target first and then moved over it, so a failed write never leaves half a DLL behind.
        bool WriteFile(const fs::path& target, const Payload& p, std::wstring& error)
        {
            if (!p.data || !p.size) { error = L"the installer is missing an embedded file (bad build)"; return false; }
            const fs::path tmp = target.wstring() + L".new";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) { error = L"cannot write " + tmp.wstring(); return false; }
                out.write(static_cast<const char*>(p.data), static_cast<std::streamsize>(p.size));
                if (!out) { error = L"cannot write " + tmp.wstring(); return false; }
            }
            if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = L"cannot replace " + target.wstring() + L": " + ErrorText(GetLastError());
                std::error_code ec;
                fs::remove(tmp, ec);
                return false;
            }
            return true;
        }

        bool HasSettings(const fs::path& menuDir)
        {
            std::error_code ec;
            if (!fs::is_directory(menuDir, ec)) return false;
            for (const auto& e : fs::directory_iterator(menuDir, ec)) {
                const std::wstring name = e.path().filename().wstring();   // Kept: filename() is a temporary
                if (_wcsicmp(name.c_str(), kMenuDll) != 0 && _wcsicmp(name.c_str(), kUninstaller) != 0) return true;
            }
            return false;
        }

        fs::path SelfPath()
        {
            wchar_t buf[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            return buf;
        }

        bool SamePath(const fs::path& a, const fs::path& b)
        {
            std::error_code ec;
            return fs::equivalent(a, b, ec);
        }

        void SetString(HKEY key, const wchar_t* name, const std::wstring& value)
        {
            RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        }

        void SetDword(HKEY key, const wchar_t* name, DWORD value)
        {
            RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
        }

        std::uintmax_t FileSize(const fs::path& p)
        {
            std::error_code ec;
            const auto n = fs::file_size(p, ec);
            return ec ? 0 : n;
        }

        // Windows' list of installed apps (Settings > Apps, Programs and Features): "Mirror's Edge Menu", with
        // uninstall.exe (a copy of this installer) as its uninstaller.
        bool RegisterApp(const fs::path& gameDir, std::wstring& error)
        {
            const fs::path menuDir = gameDir / kMenuDir;
            const fs::path uninstaller = menuDir / kUninstaller;
            const fs::path self = SelfPath();
            if (!SamePath(self, uninstaller) && !CopyFileW(self.c_str(), uninstaller.c_str(), FALSE)) {
                error = L"cannot copy the uninstaller to " + uninstaller.wstring() + L": " + ErrorText(GetLastError());
                return false;
            }

            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kAppKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
                ERROR_SUCCESS) {
                error = L"cannot add Mirror's Edge Menu to the installed apps (registry)";
                return false;
            }
            const std::wstring quoted = L"\"" + uninstaller.wstring() + L"\"";
            const std::wstring dir = L" \"" + gameDir.wstring() + L"\"";
            wchar_t date[16]{};
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            std::wcsftime(date, std::size(date), L"%Y%m%d", &tm);
            const auto kb = static_cast<DWORD>((FileSize(menuDir / kMenuDll) + FileSize(gameDir / kLoader) +
                                                FileSize(uninstaller)) / 1024);

            SetString(key, L"DisplayName", L"Mirror's Edge Menu");
            SetString(key, L"DisplayVersion", Widen(Version()));
            SetString(key, L"Publisher", L"medge-menu");
            SetString(key, L"DisplayIcon", (gameDir / kGameExe).wstring() + L",0");
            SetString(key, L"InstallLocation", menuDir.wstring());
            SetString(key, L"InstallDate", date);
            SetString(key, L"UninstallString", quoted + L" --uninstall" + dir);
            SetString(key, L"QuietUninstallString", quoted + L" --uninstall" + dir + L" --quiet");
            SetString(key, L"URLInfoAbout", L"https://github.com/connorfrancey/memenu");
            SetString(key, L"Comments", L"Trainer, TAS and tools menu for Mirror's Edge. Installed in " + gameDir.wstring());
            SetDword(key, L"EstimatedSize", kb);
            SetDword(key, L"NoModify", 1);
            SetDword(key, L"NoRepair", 1);
            RegCloseKey(key);
            return true;
        }

        void UnregisterApp() { RegDeleteKeyW(HKEY_LOCAL_MACHINE, kAppKey); }

        // A running exe can't delete itself: a hidden cmd waits for it to exit, then deletes it (and the folder,
        // if nothing else is left in it).
        void DeleteAfterExit(const fs::path& exe, const fs::path& dir)
        {
            std::wstring cmd = L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + exe.wstring() + L"\" & rmdir \"" +
                               dir.wstring() + L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                               nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }

    const char* Version() { return MEMENU_VERSION; }

    void SetAppRegistration(bool on) { g_registerApp = on; }

    std::optional<fs::path> NormalizeGameDir(const fs::path& picked)
    {
        std::error_code ec;
        fs::path p = picked;
        if (fs::is_regular_file(p, ec)) p = p.parent_path();
        for (const fs::path& dir : {p, p / L"Binaries"})
            if (fs::is_regular_file(dir / kGameExe, ec)) return dir;
        return std::nullopt;
    }

    std::optional<fs::path> DetectGameDir()
    {
        for (const auto& found : detect::FindInstalls(kGameExe))
            if (auto dir = NormalizeGameDir(found.exe)) return dir;
        return std::nullopt;
    }

    Status Inspect(const fs::path& gameDir)
    {
        Status s;
        std::error_code ec;
        s.gameFound = fs::is_regular_file(gameDir / kGameExe, ec);
        s.gameRunning = GameRunning();
        if (!s.gameFound) return s;

        const fs::path loader = gameDir / kLoader;
        if (fs::exists(loader, ec)) {
            if (auto v = LoaderVersion(loader)) s.installedVersion = *v;
            else s.foreignLoader = true;
        }
        s.installed = !s.installedVersion.empty() && fs::is_regular_file(gameDir / kMenuDir / kMenuDll, ec);
        s.hasSettings = HasSettings(gameDir / kMenuDir);
        return s;
    }

    bool Install(const fs::path& gameDir, std::wstring& message)
    {
        const Status s = Inspect(gameDir);
        if (!s.gameFound) { message = L"MirrorsEdge.exe is not in " + gameDir.wstring(); return false; }
        if (s.gameRunning) { message = L"Mirror's Edge is running. Close the game, then try again."; return false; }

        std::error_code ec;
        const fs::path menuDir = gameDir / kMenuDir;
        fs::create_directories(menuDir, ec);
        if (ec) { message = L"Cannot create " + menuDir.wstring() + L": " + Widen(ec.message()); return false; }

        // Someone else's dinput8.dll (another mod): keep it, it comes back on uninstall.
        std::wstring note;
        if (s.foreignLoader) {
            const fs::path backup = gameDir / kBackup;
            if (!fs::exists(backup, ec)) {
                fs::rename(gameDir / kLoader, backup, ec);
                if (ec) { message = L"Cannot back up the existing dinput8.dll: " + Widen(ec.message()); return false; }
            }
            note = L"\n\nThe dinput8.dll that was there (from another mod) was saved as " + std::wstring(kBackup) +
                   L" and is put back when you uninstall.";
        }

        std::wstring error;
        if (!WriteFile(menuDir / kMenuDll, Resource(IDR_MENU_DLL), error) ||
            !WriteFile(gameDir / kLoader, Resource(IDR_LOADER_DLL), error)) {
            message = L"Install failed: " + error;
            return false;
        }
        // Listed in Windows' installed apps. Not being able to is no reason to undo the install: say so instead.
        std::wstring appNote;
        if (g_registerApp && !RegisterApp(gameDir, error))
            appNote = L"\n\n(" + error + L". Uninstall by running this installer again.)";

        message = std::wstring(s.installed ? L"Updated" : L"Installed") + L" Mirror's Edge Menu " + Widen(Version()) +
                  L".\n\nStart Mirror's Edge the way you always do (Steam, EA app, shortcut). The menu loads with the "
                  L"game: press INSERT to open it, END to unload it.\n\nTo uninstall: Windows Settings > Apps > "
                  L"Mirror's Edge Menu, or run this installer again." + note + appNote;
        return true;
    }

    void Launch(const fs::path& gameDir)
    {
        std::wstring lower = gameDir.wstring();
        for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const std::wstring target = lower.find(L"\\steamapps\\") != std::wstring::npos
                                        ? L"steam://rungameid/" + std::to_wstring(detect::kSteamAppId)
                                        : (gameDir / kGameExe).wstring();
        // Through Explorer: it runs as the user, so the game doesn't inherit the installer's admin rights.
        const std::wstring args = L"\"" + target + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    bool Uninstall(const fs::path& gameDir, bool removeSettings, std::wstring& message)
    {
        const Status s = Inspect(gameDir);
        if (s.gameRunning) { message = L"Mirror's Edge is running. Close the game, then try again."; return false; }

        std::error_code ec;
        const fs::path loader = gameDir / kLoader;
        if (!s.installedVersion.empty()) {
            fs::remove(loader, ec);
            if (ec) { message = L"Cannot remove " + loader.wstring() + L": " + Widen(ec.message()); return false; }
        }
        if (fs::exists(gameDir / kBackup, ec) && !fs::exists(loader, ec)) fs::rename(gameDir / kBackup, loader, ec);

        const fs::path menuDir = gameDir / kMenuDir;
        const fs::path uninstaller = menuDir / kUninstaller;
        const bool runningFromThere = SamePath(SelfPath(), uninstaller);
        std::wstring failed;
        const auto remove = [&](const fs::path& p) {
            std::error_code e;
            fs::remove_all(p, e);
            if (e && failed.empty()) failed = p.filename().wstring() + L": " + Widen(e.message());
        };
        if (removeSettings) {
            std::vector<fs::path> entries;      // listed first: not deleted while the folder is being read
            for (const auto& entry : fs::directory_iterator(menuDir, ec)) entries.push_back(entry.path());
            for (const fs::path& entry : entries)
                if (!(runningFromThere && SamePath(entry, uninstaller))) remove(entry);
        } else {
            remove(menuDir / kMenuDll);
            if (!runningFromThere) remove(uninstaller);
        }
        if (g_registerApp) UnregisterApp();
        if (runningFromThere) DeleteAfterExit(uninstaller, menuDir);   // this exe: once it has exited
        else if (removeSettings) fs::remove(menuDir, ec);
        if (!failed.empty()) {
            message = L"Some files could not be removed from " + menuDir.wstring() + L" (" + failed + L")";
            return false;
        }

        message = removeSettings ? L"Mirror's Edge Menu was removed, with its settings."
                                 : L"Mirror's Edge Menu was removed. Your settings were kept in " + menuDir.wstring() + L".";
        return true;
    }
}
