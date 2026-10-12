#pragma once
#include <Windows.h>
#include <filesystem>

namespace paths
{
    inline std::filesystem::path g_dir;

    // Remember the folder the DLL lives in (log + config are stored there).
    inline void Init(HMODULE module)
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(module, buf, MAX_PATH);
        g_dir = std::filesystem::path(buf).parent_path();
    }
    inline const std::filesystem::path& Dir() { return g_dir; }
}
