#pragma once
// Diagnostics for memenu.log: what the process looks like when the menu attaches, and where things crash.
// The helpers in this header are inline so hooks.cpp can use them in the staged builds too (needs spdlog/fmt);
// the functions declared at the bottom are in diag.cpp (full menu build only).
#include <Windows.h>
#include <spdlog/fmt/fmt.h>

#include <cstdint>
#include <string>

namespace diag {
    inline std::string Narrow(const wchar_t* w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string s(static_cast<std::size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    inline std::string ModulePath(HMODULE m)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(m, path, MAX_PATH);
        return Narrow(path);
    }

    // "d3d9.dll+0x1A2B0": which module an address is in. With a .pdb, module+offset finds the source line.
    inline std::string DescribeAddress(const void* addr)
    {
        HMODULE m = nullptr;
        if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         static_cast<LPCWSTR>(addr), &m))
            return fmt::format("{} (not in a module)", addr);
        const std::string path = ModulePath(m);
        const std::size_t slash = path.find_last_of("\\/");
        return fmt::format("{}+0x{:X}", slash == std::string::npos ? path : path.substr(slash + 1),
                           reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(m));
    }

    // --- diag.cpp ---
    void LogStartup(HMODULE self);          // Process, OS, this DLL and the other modules loaded in the game
    void InstallCrashLogger(HMODULE self);  // Logs (and flushes) hardware exceptions raised inside this DLL
    void RemoveCrashLogger();
    long LogFatal(const EXCEPTION_POINTERS* ep, const char* where);   // For __except filters: logs, keeps searching
}
