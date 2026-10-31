#include "detect.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace detect
{
    namespace
    {
        // REG_SZ value, or nothing. `view` is KEY_WOW64_32KEY / KEY_WOW64_64KEY (ignored for HKCU keys that aren't redirected).
        std::optional<std::wstring> RegString(HKEY root, const wchar_t* subKey, const wchar_t* value, REGSAM view = 0)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, subKey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return std::nullopt;

            std::optional<std::wstring> result;
            DWORD type = 0, bytes = 0;
            if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ) && bytes > 0)
            {
                std::wstring s(bytes / sizeof(wchar_t), L'\0');
                if (RegQueryValueExW(key, value, nullptr, nullptr, reinterpret_cast<BYTE*>(s.data()), &bytes) == ERROR_SUCCESS)
                {
                    while (!s.empty() && s.back() == L'\0') s.pop_back();
                    if (!s.empty()) result = std::move(s);
                }
            }
            RegCloseKey(key);
            return result;
        }

        std::string ReadFile(const fs::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        std::wstring Widen(const std::string& s)
        {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // All values of `"key"  "value"` pairs in a Valve KeyValues (.vdf/.acf) file, with \\ unescaped.
        std::vector<std::wstring> VdfValues(const std::string& text, const std::string& key)
        {
            std::vector<std::wstring> out;
            const std::regex re("\"" + key + "\"\\s+\"((?:[^\"\\\\]|\\\\.)*)\"", std::regex::icase);
            for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
            {
                std::string v = (*it)[1].str();
                std::string unescaped;
                for (std::size_t i = 0; i < v.size(); ++i)
                {
                    if (v[i] == '\\' && i + 1 < v.size()) ++i;
                    unescaped += v[i];
                }
                out.push_back(Widen(unescaped));
            }
            return out;
        }

        // The game exe inside an install folder: <dir>\Binaries\MirrorsEdge.exe (or directly in <dir>).
        std::optional<fs::path> ExeIn(const fs::path& dir, const std::wstring& exeName)
        {
            std::error_code ec;
            for (const fs::path& candidate : {dir / L"Binaries" / exeName, dir / exeName})
                if (fs::is_regular_file(candidate, ec)) return candidate;
            return std::nullopt;
        }

        // ---- Steam --------------------------------------------------------------------------------------------------
        std::vector<fs::path> SteamLibraries()
        {
            std::vector<fs::path> libs;
            std::optional<std::wstring> steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
            if (!steam) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
            if (!steam) return libs;

            const fs::path root = fs::path(*steam).make_preferred();
            libs.push_back(root);
            for (const fs::path& vdf : {root / L"steamapps" / L"libraryfolders.vdf", root / L"config" / L"libraryfolders.vdf"})
            {
                const std::string text = ReadFile(vdf);
                for (const std::wstring& p : VdfValues(text, "path")) libs.push_back(fs::path(p).make_preferred());
                if (!text.empty()) break;
            }
            return libs;
        }

        void FindSteam(const std::wstring& exeName, std::vector<Install>& out)
        {
            for (const fs::path& lib : SteamLibraries())
            {
                const fs::path manifest = lib / L"steamapps" / (L"appmanifest_" + std::to_wstring(kSteamAppId) + L".acf");
                const std::string text = ReadFile(manifest);
                if (text.empty()) continue;
                const auto dirs = VdfValues(text, "installdir");
                if (dirs.empty()) continue;
                if (const auto exe = ExeIn(lib / L"steamapps" / L"common" / dirs.front(), exeName))
                    out.push_back({*exe, kSteamAppId, L"Steam library " + lib.wstring()});
            }
        }

        // ---- EA / Origin / retail -----------------------------------------------------------------------------------
        void FindEaRegistry(const std::wstring& exeName, std::vector<Install>& out)
        {
            for (const wchar_t* key : {L"SOFTWARE\\EA Games\\Mirror's Edge", L"SOFTWARE\\EA Games\\Mirrors Edge"})
                for (const wchar_t* value : {L"Install Dir", L"InstallDir"})
                    if (const auto dir = RegString(HKEY_LOCAL_MACHINE, key, value, KEY_WOW64_32KEY))
                        if (const auto exe = ExeIn(*dir, exeName)) out.push_back({*exe, std::nullopt, L"EA registry key"});
        }

        bool LooksLikeMirrorsEdge(std::wstring name)
        {
            for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
            name.erase(std::remove(name.begin(), name.end(), L'\''), name.end());
            return name.find(L"mirrors edge") != std::wstring::npos && name.find(L"catalyst") == std::wstring::npos;
        }

        void FindUninstallEntries(const std::wstring& exeName, std::vector<Install>& out)
        {
            constexpr const wchar_t* kUninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
            for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY})
            {
                HKEY key = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstall, 0, KEY_READ | view, &key) != ERROR_SUCCESS) continue;
                wchar_t sub[256];
                for (DWORD i = 0;; ++i)
                {
                    DWORD len = static_cast<DWORD>(std::size(sub));
                    if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    const std::wstring path = std::wstring(kUninstall) + L"\\" + sub;
                    const auto name = RegString(HKEY_LOCAL_MACHINE, path.c_str(), L"DisplayName", view);
                    if (!name || !LooksLikeMirrorsEdge(*name)) continue;
                    const auto dir = RegString(HKEY_LOCAL_MACHINE, path.c_str(), L"InstallLocation", view);
                    if (!dir) continue;
                    if (const auto exe = ExeIn(*dir, exeName))
                    {
                        // Steam registers its games here too ("Steam App 17410"): those must still start through Steam.
                        const bool steam = std::wstring(sub).rfind(L"Steam App ", 0) == 0;
                        out.push_back({*exe, steam ? std::optional<unsigned>(kSteamAppId) : std::nullopt,
                                       L"installed programs (" + *name + L")"});
                    }
                }
                RegCloseKey(key);
            }
        }

        bool SameFile(const fs::path& a, const fs::path& b)
        {
            std::error_code ec;
            return fs::equivalent(a, b, ec);
        }
    }

    std::vector<Install> FindInstalls(const std::wstring& exeName)
    {
        std::vector<Install> found;
        FindSteam(exeName, found);
        FindEaRegistry(exeName, found);
        FindUninstallEntries(exeName, found);

        std::vector<Install> unique;
        for (Install& i : found)
        {
            const auto dup = std::find_if(unique.begin(), unique.end(), [&](const Install& u) { return SameFile(u.exe, i.exe); });
            if (dup == unique.end()) unique.push_back(std::move(i));
            else if (!dup->steamAppId && i.steamAppId) dup->steamAppId = i.steamAppId;  // Inside a Steam library after all
        }
        return unique;
    }
}
