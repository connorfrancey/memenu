#pragma once
// Debug-build checks (assert, IM_ASSERT, /RTC run-time checks) normally stop with a dialog. That dialog says what
// failed but nothing records it, and an unattended test would wait on it forever. This sends the message to the log
// first. In test mode (environment variable MEMENU_TEST_MODE=1 in the process, set by the test host of
// `memenu verbose-build`) there is no dialog either: the check breaks right away, which the host catches as a crash
// and writes a dump for. Header-only: the staged build's bare stages use it too. Release builds: does nothing.
#include <Windows.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <rtcapi.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#endif

namespace testmode {
    using LogFn = void (*)(const char* message);

    inline bool Enabled()
    {
        char v[8]{};
        return GetEnvironmentVariableA("MEMENU_TEST_MODE", v, sizeof v) > 0 && v[0] == '1';
    }

#if defined(_MSC_VER) && defined(_DEBUG)
    inline LogFn& Log() { static LogFn fn = nullptr; return fn; }

    inline int __cdecl CrtReport(int type, char* message, int* returnValue)
    {
        if (type == _CRT_WARN) return FALSE;   // _RPT0(_CRT_WARN...) chatter: leave it to the debugger output
        if (Log() && message) Log()(message);
        if (!Enabled()) return FALSE;          // Normal: the usual dialog
        if (returnValue) *returnValue = 1;     // Test mode: break now (the test host catches it)
        return TRUE;
    }

    inline int __cdecl RtcError(int type, const wchar_t* file, int line, const wchar_t* module, const wchar_t* format, ...)
    {
        wchar_t text[512]{};
        va_list args;
        va_start(args, format);
        std::vswprintf(text, 512, format, args);
        va_end(args);
        char message[1024];
        std::snprintf(message, sizeof message, "Run-Time Check Failure %d: %ls (%ls:%d, %ls)", type, text,
                      file ? file : L"?", line, module ? module : L"?");
        if (Log()) Log()(message);
        return 1;   // Break into the debugger (no debugger: the crash the old /RTC dialog led to anyway)
    }
#endif

    // `log` gets each failed check's message (from any thread; keep it simple).
    inline void Install(LogFn log)
    {
#if defined(_MSC_VER) && defined(_DEBUG)
        Log() = log;
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CrtReport);
#pragma warning(suppress : 4996)   // _RTC_SetErrorFuncW is deprecated but is the only way to see /RTC failures
        _RTC_SetErrorFuncW(RtcError);
#else
        (void)log;
#endif
    }

    inline void Uninstall()
    {
#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, CrtReport);
#pragma warning(suppress : 4996)
        _RTC_SetErrorFuncW(nullptr);
        Log() = nullptr;
#endif
    }
}
