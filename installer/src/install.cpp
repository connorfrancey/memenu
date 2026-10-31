#include "install.h"
#include "resource.h"

#include "detect.h"

#include <Windows.h>
#include <TlHelp32.h>

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
            for (const auto& e : fs::directory_iterator(menuDir, ec))
                if (_wcsicmp(e.path().filename().c_str(), kMenuDll) != 0) return true;
            return false;
        }
    }

    const char* Version() { return MEMENU_VERSION; }

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

        message = std::wstring(s.installed ? L"Updated" : L"Installed") + L" Mirror's Edge Menu " + Widen(Version()) +
                  L".\n\nStart Mirror's Edge the way you always do (Steam, EA app, shortcut). The menu loads with the "
                  L"game: press INSERT to open it, END to unload it." + note;
        return true;
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
        if (removeSettings) fs::remove_all(menuDir, ec);
        else fs::remove(menuDir / kMenuDll, ec);
        if (ec) { message = L"Some files could not be removed from " + menuDir.wstring() + L": " + Widen(ec.message()); return false; }

        message = removeSettings ? L"Mirror's Edge Menu was removed, with its settings."
                                 : L"Mirror's Edge Menu was removed. Your settings were kept in " + menuDir.wstring() + L".";
        return true;
    }
}
