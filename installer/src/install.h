#pragma once
#include <filesystem>
#include <optional>
#include <string>

// Installs the menu into a Mirror's Edge Binaries folder (the one with MirrorsEdge.exe):
//
//   Binaries\dinput8.dll              the loader (loads the menu with the game)
//   Binaries\memenu\medge_menu.dll    the menu
//   Binaries\memenu\...               its config, themes, cache and logs (created by the menu)
//
// Both DLLs are embedded in the installer. A dinput8.dll from another mod is kept as dinput8.dll.memenu-backup and put
// back on uninstall.
namespace install
{
    struct Status
    {
        bool gameFound = false;                     // The folder has MirrorsEdge.exe
        bool gameRunning = false;
        bool installed = false;                     // Our loader and the menu are there
        std::string installedVersion;               // Of the loader that is there, "" if none
        bool foreignLoader = false;                 // A dinput8.dll that isn't ours
        bool hasSettings = false;                   // memenu\ has files besides the menu DLL
    };

    const char* Version();                          // Of the files embedded in this installer

    // Folder with MirrorsEdge.exe of the best install found on this machine (Steam, EA, installed programs).
    std::optional<std::filesystem::path> DetectGameDir();

    // Accepts the exe, the Binaries folder or the game's root folder; returns the Binaries folder (or nothing).
    std::optional<std::filesystem::path> NormalizeGameDir(const std::filesystem::path& picked);

    Status Inspect(const std::filesystem::path& gameDir);

    // On failure `message` says why. On success it says what was done.
    bool Install(const std::filesystem::path& gameDir, std::wstring& message);
    bool Uninstall(const std::filesystem::path& gameDir, bool removeSettings, std::wstring& message);
}
