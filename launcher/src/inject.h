#pragma once
#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace inject
{
    struct Result
    {
        bool ok = false;
        std::wstring message;
    };

    // Case-insensitive match on the executable name, e.g. L"MirrorsEdge.exe".
    std::vector<DWORD> FindProcesses(const std::wstring& exeName);
    std::optional<DWORD> FindProcess(const std::wstring& exeName);

    // Is a module with this file name (e.g. L"medge_menu.dll") already loaded in the process?
    bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName);

    // IMAGE_FILE_HEADER.Machine of a PE file (0x14C = x86), or nothing if it isn't a PE file.
    std::optional<WORD> PeMachine(const std::filesystem::path& file);

    // Classic injection: write the DLL path into the target and run LoadLibraryW there.
    Result LoadLibraryInto(DWORD pid, const std::filesystem::path& dll);
}