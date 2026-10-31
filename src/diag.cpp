#include "diag.h"

#include <spdlog/spdlog.h>
#include <Psapi.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace {
    HMODULE g_self = nullptr;
    std::uintptr_t g_selfBegin = 0, g_selfEnd = 0;
    PVOID g_veh = nullptr;

    bool InSelf(const void* addr)
    {
        const auto p = reinterpret_cast<std::uintptr_t>(addr);
        return p >= g_selfBegin && p < g_selfEnd;
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string FileName(const std::string& path)
    {
        const std::size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string OsVersion()
    {
        using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        std::string out = "unknown";
        RTL_OSVERSIONINFOW v{sizeof v};
        if (auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion")); fn && fn(&v) == 0)
            out = fmt::format("Windows {}.{} build {}", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        using WineVersion_t = const char*(__cdecl*)();
        if (auto wine = reinterpret_cast<WineVersion_t>(GetProcAddress(ntdll, "wine_get_version")))
            out += fmt::format(" (Wine/Proton {})", wine());
        return out;
    }

    bool IsElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
        CloseHandle(token);
        return ok;
    }

    // Modules that hook D3D9 or the window themselves; any of them can fight with the menu's hooks.
    const char* KnownHooker(const std::string& lowerName)
    {
        static const std::pair<const char*, const char*> kKnown[] = {
            {"gameoverlayrenderer", "Steam overlay"},   {"discordhook", "Discord overlay"},
            {"rtsshooks", "RivaTuner/MSI Afterburner"}, {"reshade", "ReShade"},
            {"dxvk", "DXVK"},                           {"graphics-hook", "OBS game capture"},
            {"nvspcap", "NVIDIA ShadowPlay"},           {"overlay", "an overlay"},
            {"medge_menu", "another copy of this menu"}, {"mmultiplayer", "mmultiplayer"},
        };
        for (const auto& [needle, what] : kKnown)
            if (lowerName.find(needle) != std::string::npos) return what;
        return nullptr;
    }

    void LogModules()
    {
        std::vector<HMODULE> mods(512);
        DWORD needed = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed))
        {
            spdlog::warn("EnumProcessModules failed (error {})", GetLastError());
            return;
        }
        mods.resize(std::min<std::size_t>(mods.size(), needed / sizeof(HMODULE)));

        wchar_t winDirW[MAX_PATH]{};
        GetWindowsDirectoryW(winDirW, MAX_PATH);
        const std::string winDir = Lower(diag::Narrow(winDirW));
        const std::string selfName = Lower(FileName(diag::ModulePath(g_self)));

        // Everything at debug level; at info only what is not part of Windows (game DLLs, wrappers, overlays)
        int outside = 0;
        for (HMODULE m : mods)
        {
            const std::string path = diag::ModulePath(m);
            const std::string lower = Lower(path);
            const std::string name = Lower(FileName(path));
            const bool system = lower.rfind(winDir, 0) == 0;
            const char* hooker = m == g_self ? nullptr : KnownHooker(name);

            if (m != g_self && name == selfName)
                spdlog::warn("Module: {} is ANOTHER COPY of this menu, already loaded: unload it (END) before injecting again", path);
            else if (hooker)
                spdlog::warn("Module: {} ({}: it hooks rendering too and may conflict with the menu)", path, hooker);
            else if (name == "d3d9.dll" && !system)
                spdlog::warn("Module: {} is a d3d9.dll WRAPPER (not the Windows one): the dummy device and hooks go through it", path);
            else if (!system)
                spdlog::info("Module: {} (base {})", path, static_cast<void*>(m));
            else
                spdlog::debug("Module: {} (base {})", path, static_cast<void*>(m));
            outside += system ? 0 : 1;
        }
        spdlog::info("{} modules loaded, {} outside {}", mods.size(), outside, winDir);
    }

    LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep)
    {
        switch (ep->ExceptionRecord->ExceptionCode)
        {
            case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_IN_PAGE_ERROR:
                break;
            default:    // Not a crash (C++ exceptions, debugger events...), or a stack overflow we can't log from
                return EXCEPTION_CONTINUE_SEARCH;
        }
        if (!InSelf(ep->ExceptionRecord->ExceptionAddress)) return EXCEPTION_CONTINUE_SEARCH;   // The game's own business
        static thread_local bool inside = false;
        if (inside) return EXCEPTION_CONTINUE_SEARCH;
        inside = true;
        diag::LogFatal(ep, "Crash in the menu");
        inside = false;
        return EXCEPTION_CONTINUE_SEARCH;   // Only logs: the game's own handling (crash dialog) still happens
    }
}

void diag::LogStartup(HMODULE self)
{
    g_self = self;
    MODULEINFO mi{};
    if (K32GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof mi))
    {
        g_selfBegin = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
        g_selfEnd = g_selfBegin + mi.SizeOfImage;
    }

    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    spdlog::info("Process: {} (pid {}, {}{})", ModulePath(nullptr), GetCurrentProcessId(),
                 wow64 ? "32-bit on 64-bit Windows" : "native", IsElevated() ? ", elevated" : "");
    spdlog::info("Command line: {}", Narrow(GetCommandLineW()));
    spdlog::info("OS: {}", OsVersion());
    spdlog::info("Menu DLL: {} (base {}, {} KB), menu thread {}", ModulePath(self), static_cast<void*>(self),
                 mi.SizeOfImage / 1024, GetCurrentThreadId());
    LogModules();
}

void diag::InstallCrashLogger(HMODULE self)
{
    if (!g_self) g_self = self;
    g_veh = AddVectoredExceptionHandler(1, CrashLogger);
    if (!g_veh) spdlog::warn("Could not install the crash logger");
}

void diag::RemoveCrashLogger()
{
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
}

long diag::LogFatal(const EXCEPTION_POINTERS* ep, const char* where)
{
    static thread_local const void* lastLogged = nullptr;   // The crash logger may have logged this one already
    if (ep->ExceptionRecord == lastLogged) return EXCEPTION_CONTINUE_SEARCH;
    lastLogged = ep->ExceptionRecord;

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    std::string detail;
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er->NumberParameters >= 2)
    {
        const ULONG_PTR op = er->ExceptionInformation[0];
        detail = fmt::format(" ({} {})", op == 0 ? "reading" : op == 1 ? "writing" : op == 8 ? "executing" : "accessing",
                             reinterpret_cast<void*>(er->ExceptionInformation[1]));
    }
    else if (er->ExceptionCode == 0xE06D7363)
    {
        detail = " (uncaught C++ exception)";
    }
    spdlog::critical("{}: exception 0x{:08X}{} at {} (thread {})", where, static_cast<unsigned long>(er->ExceptionCode),
                     detail, DescribeAddress(er->ExceptionAddress), GetCurrentThreadId());

#if defined(_M_IX86)
    const CONTEXT* c = ep->ContextRecord;
    spdlog::critical("  eax={:08X} ebx={:08X} ecx={:08X} edx={:08X} esi={:08X} edi={:08X} ebp={:08X} esp={:08X}",
                     c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // Call stack through the frame pointer chain (complete in debug builds; release builds may skip frames).
    // Resolve module+offset with the .pdb (e.g. in Visual Studio: Debug > Windows > Disassembly, go to address).
    spdlog::critical("  #0 {}", DescribeAddress(er->ExceptionAddress));
    std::uintptr_t ebp = c->Ebp;
    for (int i = 1; i < 32 && ebp; ++i)
    {
        std::uintptr_t frame[2]{};   // [saved ebp, return address]
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ebp), frame, sizeof frame, &read) ||
            read != sizeof frame || !frame[1])
            break;
        spdlog::critical("  #{} {}", i, DescribeAddress(reinterpret_cast<void*>(frame[1])));
        if (frame[0] <= ebp) break;   // Caller frames are higher up the stack
        ebp = frame[0];
    }
#endif
    if (auto logger = spdlog::default_logger()) logger->flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
