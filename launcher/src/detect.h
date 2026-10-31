#pragma once
#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Finds a Mirror's Edge install on this machine, so the launcher can start the game without being told where it is.
namespace detect
{
    constexpr unsigned kSteamAppId = 17410;

    struct Install
    {
        std::filesystem::path exe;              // ...\Binaries\MirrorsEdge.exe
        std::optional<unsigned> steamAppId;     // Set for Steam copies: start them through Steam
        std::wstring source;                    // Where it was found ("Steam", "EA registry", ...), for the log
    };

    // Every install that was found, best first (Steam, then the EA registry key, then the Windows uninstall list).
    // Duplicates (the same exe found twice) are removed.
    std::vector<Install> FindInstalls(const std::wstring& exeName = L"MirrorsEdge.exe");
}
