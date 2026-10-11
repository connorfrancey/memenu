#include "inject.h"

#include <TlHelp32.h>

#include <cwchar>
#include <fstream>
#include <memory>

namespace inject
{
    namespace
    {
        struct HandleCloser
        {
            void operator()(HANDLE h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;

        std::wstring ErrorText(DWORD code)
        {
            wchar_t buf[256]{};
            const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                buf, static_cast<DWORD>(std::size(buf)), nullptr);
            std::wstring s(buf, n);
            while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
            return s + L" (error " + std::to_wstring(code) + L")";
        }

        Result Fail(std::wstring msg) {return { false, std::move(msg)}; }

        // The launcher is 32-bit. On 64-bit Windows both it and a 32-bit target report WOW64 = TRUE,
        // on 32-bit Windows both report FALSE. A mismatch means the target is a 64-bit process.
        bool SameBitnessAsLauncher(HANDLE process)
        {
            BOOL self = FALSE, target = FALSE;
            IsWow64Process(GetCurrentProcess(), &self);
            IsWow64Process(process, &target);
            return self == target;
        }
    }

    std::vector<DWORD> FindProcesses(const std::wstring& exeName)
    {
        std::vector<DWORD> pids;
        const HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (s == INVALID_HANDLE_VALUE) return pids;
        Handle snap(s);

        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) pids.push_back(pe.th32ProcessID);
        return pids;
    }

    std::optional<DWORD> FindProcess(const std::wstring& exeName)
    {
        const auto pids = FindProcesses(exeName);
        if (pids.empty()) return std::nullopt;
        return pids.front();
    }

    bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName)
    {
        const HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (s == INVALID_HANDLE_VALUE) return false;
        Handle snap(s);

        MODULEENTRY32W me{ sizeof(me) };
        for (BOOL ok = Module32FirstW(s, &me); ok; ok = Module32NextW(s, &me))
            if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) return true;
        return false;
    }

    std::optional<WORD> PeMachine(const std::filesystem::path& file)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) return std::nullopt;

        IMAGE_DOS_HEADER dos{};
        f.read(reinterpret_cast<char*>(&dos), sizeof dos);
        if (!f || dos.e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;

        f.seekg(dos.e_lfanew);
        DWORD sig = 0;
        IMAGE_FILE_HEADER fh{};
        f.read(reinterpret_cast<char*>(&sig), sizeof sig);
        f.read(reinterpret_cast<char*>(&fh), sizeof fh);
        if (!f || sig != IMAGE_NT_SIGNATURE) return std::nullopt;
        return fh.Machine;
    }

    Result LoadLibraryInto(DWORD pid, const std::filesystem::path& dll)
    {
        const Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid));
        if (!process)
        {
            const DWORD err = GetLastError();
            std::wstring msg = L"OpenProcess failed: " + ErrorText(err);
            if (err == ERROR_ACCESS_DENIED)
                msg += L"\nThe game is probably running elevated: start the launcher as Administrator too.";
            return Fail(std::move(msg));
        }
        if (!SameBitnessAsLauncher(process.get()))
            return Fail(L"The target is a 64-bit process, but the launcher (and the DLL) are 32-bit.");

        const std::wstring path = std::filesystem::absolute(dll).wstring();
        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);

        void* remote = VirtualAllocEx(process.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) return Fail(L"VirtualAllocEx failed: " + ErrorText(GetLastError()));

        if (!WriteProcessMemory(process.get(), remote, path.c_str(), bytes, nullptr))
        {
            const std::wstring msg = L"WriteProcessMemory failed: " + ErrorText(GetLastError());
            VirtualFreeEx(process.get(), remote, 0, MEM_RELEASE);
            return Fail(msg);
        }

        // kernal32 is mapped at the same address in every process of the same bitness, so our own
        // address of LoadLibraryW is valid in the game too.
        const FARPROC loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        const Handle thread(CreateRemoteThread(process.get(), nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(loadLibrary)),
            remote, 0, nullptr));
        if (!thread)
        {
            const std::wstring msg = L"CreateRemoteThread failed: " + ErrorText(GetLastError());
            VirtualFreeEx(process.get(), remote, 0, MEM_RELEASE);
            return Fail(msg);
        }

        if (WaitForSingleObject(thread.get(), 10000) != WAIT_OBJECT_0)
            return Fail(L"Timed out waiting for LoadLibraryW in the game (path buffer left allocated).");

        DWORD module = 0;
        GetExitCodeThread(thread.get(), &module);
        VirtualFreeEx(process.get(), remote, 0, MEM_RELEASE);

        if (module == 0)
            return Fail(L"LoadLibraryW returned NULL inside the game. Usual causes: the DLL file is blocked or missing, "
                        L"it needs a runtime/DLL that isn't present, or its DllMain failed.");
        return {true, L"Injected " + path};
    }


}
