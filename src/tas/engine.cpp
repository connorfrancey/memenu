#include "tas/engine.h"
#include "game/game.h"
#include "game/object_ref.h"

#include <Windows.h>

#include <MinHook.h>
#include <ME_SDK.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace tas::engine {

namespace {

// While the virtual clock runs, the game thread sees a 10 MHz performance counter (what mtas uses too): the same tick
// values on every machine, whatever its real counter frequency.
constexpr long long kVirtualFrequency = 10'000'000;

std::atomic<bool> g_available{false};
std::atomic<DWORD> g_gameThread{0};

// Engine time globals (found next to the frame timing function, see Init)
double* g_delta = nullptr;            // this frame's delta seconds
double* g_current = nullptr;          // current time (seconds)
double* g_last = nullptr;             // previous frame's time
double* g_secondsPerCycle = nullptr;  // 1 / performance counter frequency
double  g_realSecondsPerCycle = 0.0;

FrameHandler g_frameHandler = nullptr;
InputFilter g_inputFilter = nullptr;

// Virtual clock (game thread)
std::atomic<bool> g_virtual{false};   // read by every thread that asks for the time
long long g_virtualTicks = 0;         // counter value at the start of this frame
long long g_frameTicks = 0;           // length of this frame in ticks
long long g_calls = 0;                // QueryPerformanceCounter calls this frame
double g_virtualSeconds = 0.0;

// Input
bool g_injecting = false;
void* g_lastClient = nullptr;         // `this` and viewport of the last real input call
void* g_lastViewport = nullptr;
std::uintptr_t g_inputGlobal = 0;     // mtas: **global = the viewport, client at +0x20, FViewport at +4

using Time_t = void(__fastcall*)(void*, void*);
using InputKey_t = int(__thiscall*)(void*, void*, int, int, int, int, float, int);
using InputAxis_t = int(__thiscall*)(void*, void*, int, int, int, float, float, int);
using QPC_t = BOOL(WINAPI*)(LARGE_INTEGER*);
using TickCount_t = DWORD(WINAPI*)();
using TickCount64_t = ULONGLONG(WINAPI*)();
using srand_t = void(__cdecl*)(unsigned);

Time_t oTime = nullptr;
InputKey_t oInputKey = nullptr;
InputAxis_t oInputAxis = nullptr;
QPC_t oQPC = nullptr;
QPC_t oQPF = nullptr;
TickCount_t oGetTickCount = nullptr;
TickCount64_t oGetTickCount64 = nullptr;
TickCount_t oTimeGetTime = nullptr;
srand_t g_srand = nullptr;

bool OnGameThreadVirtual()
{
    return g_virtual.load(std::memory_order_relaxed) && GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed);
}

// ---- hooks ------------------------------------------------------------------------------------------------------------
// The engine's frame timing (mtas: "UpdateEngine"). Takes no arguments; ecx/edx are passed through untouched in case it
// is a member function.
void __fastcall TimeHook(void* ecx, void* edx)
{
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_frameHandler && g_frameHandler()) return;
    oTime(ecx, edx);
}

BOOL WINAPI QpcHook(LARGE_INTEGER* out)
{
    if (out && OnGameThreadVirtual()) {
        // Moves forward a tick per call so time never stands still inside a frame, but never into the next frame.
        out->QuadPart = g_virtualTicks + g_calls;
        if (g_calls + 1 < g_frameTicks) ++g_calls;
        return TRUE;
    }
    return oQPC(out);
}

BOOL WINAPI QpfHook(LARGE_INTEGER* out)
{
    if (out && OnGameThreadVirtual()) {
        out->QuadPart = kVirtualFrequency;
        return TRUE;
    }
    return oQPF(out);
}

DWORD WINAPI GetTickCountHook()
{
    if (OnGameThreadVirtual()) return static_cast<DWORD>(g_virtualSeconds * 1000.0);
    return oGetTickCount();
}

ULONGLONG WINAPI GetTickCount64Hook()
{
    if (OnGameThreadVirtual()) return static_cast<ULONGLONG>(g_virtualSeconds * 1000.0);
    return oGetTickCount64();
}

DWORD WINAPI TimeGetTimeHook()
{
    if (OnGameThreadVirtual()) return static_cast<DWORD>(g_virtualSeconds * 1000.0);
    return oTimeGetTime();
}

int __fastcall InputKeyHook(void* self, void* /*edx*/, void* viewport, int controllerId, int nameIndex, int nameNumber,
                            int event, float amount, int gamepad)
{
    if (!g_injecting) {
        g_lastClient = self;
        g_lastViewport = viewport;
        if (g_inputFilter) {
            RawInput in;
            in.nameIndex = nameIndex;
            in.nameNumber = nameNumber;
            in.event = event;
            in.value = amount;
            in.gamepad = gamepad != 0;
            in.controllerId = controllerId;
            if (g_inputFilter(in)) return 0;
        }
    }
    return oInputKey(self, viewport, controllerId, nameIndex, nameNumber, event, amount, gamepad);
}

int __fastcall InputAxisHook(void* self, void* /*edx*/, void* viewport, int controllerId, int nameIndex, int nameNumber,
                             float delta, float deltaTime, int gamepad)
{
    if (!g_injecting) {
        g_lastClient = self;
        g_lastViewport = viewport;
        if (g_inputFilter) {
            RawInput in;
            in.nameIndex = nameIndex;
            in.nameNumber = nameNumber;
            in.axis = true;
            in.value = delta;
            in.value2 = deltaTime;
            in.gamepad = gamepad != 0;
            in.controllerId = controllerId;
            if (g_inputFilter(in)) return 0;
        }
    }
    return oInputAxis(self, viewport, controllerId, nameIndex, nameNumber, delta, deltaTime, gamepad);
}

// ---- helpers ----------------------------------------------------------------------------------------------------------
bool InImage(const void* p)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    return a >= base && a < base + nt->OptionalHeader.SizeOfImage;
}

bool Hook(void* target, void* detour, void** original, const char* what)
{
    if (!target || MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        spdlog::error("TAS: could not hook {}", what);
        return false;
    }
    return true;
}

bool HookApi(const wchar_t* module, const char* name, void* detour, void** original)
{
    HMODULE m = GetModuleHandleW(module);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    return Hook(target, detour, original, name);
}

template <class T>
T ReadPtr(std::uintptr_t at)
{
    if (!at || IsBadReadPtr(reinterpret_cast<const void*>(at), sizeof(T))) return T{};
    return *reinterpret_cast<const T*>(at);
}

// The viewport client and viewport to inject into: mtas's pointer chain if it resolves, else the last real call's.
bool InputTarget(void*& client, void*& viewport)
{
    if (g_inputGlobal) {
        const auto p1 = ReadPtr<std::uintptr_t>(g_inputGlobal);
        const auto viewportObj = ReadPtr<std::uintptr_t>(p1);
        const auto c = ReadPtr<std::uintptr_t>(viewportObj + 0x20);
        if (viewportObj && c) {
            client = reinterpret_cast<void*>(c);
            viewport = reinterpret_cast<void*>(viewportObj + 4);
            return true;
        }
    }
    client = g_lastClient;
    viewport = g_lastViewport;
    return client && viewport;
}

Classes::UGameViewportClient* GameViewport()
{
    static game::ObjectRef<Classes::UGameEngine> engine{"Game engine"};
    auto* e = engine.Find([](Classes::UObject* o) {
        return o->IsA(Classes::UGameEngine::StaticClass()) && !game::IsDefaultObject(o);
    });
    return e ? e->GameViewport : nullptr;
}

} // namespace

bool Init()
{
    // Frame timing and the time globals, located exactly as in mtas (MainHooks): the pattern sits inside the timing
    // function, the globals are operands of the instructions that follow it.
    auto* p = static_cast<std::uint8_t*>(game::FindPattern("\xDD\x05\x00\x00\x00\x00\x5E\xDD\x15", "xx????xxx"));
    if (!p) {
        spdlog::error("TAS: frame timing pattern not found: TAS/replays are unavailable");
        return false;
    }
    const std::uint8_t* at = p + 7;
    g_delta = *reinterpret_cast<double* const*>(at + 2);
    g_current = *reinterpret_cast<double* const*>(at + 8);
    g_last = *reinterpret_cast<double* const*>(at + 14);
    g_secondsPerCycle = *reinterpret_cast<double* const*>(at + 49);
    void* timeFn = const_cast<std::uint8_t*>(at - 78);
    if (!InImage(g_delta) || !InImage(g_current) || !InImage(g_last) || !InImage(g_secondsPerCycle)) {
        spdlog::error("TAS: the time globals don't point into the game: unsupported game version?");
        return false;
    }
    g_realSecondsPerCycle = *g_secondsPerCycle;
    spdlog::info("TAS: timing at {}, delta {} current {} last {} seconds/cycle {} ({:.3g})", timeFn,
                 static_cast<void*>(g_delta), static_cast<void*>(g_current), static_cast<void*>(g_last),
                 static_cast<void*>(g_secondsPerCycle), g_realSecondsPerCycle);

    void* inputKey = game::FindPattern("\x51\x53\x8B\x5C\x24\x24\x55\x8B\x6C\x24\x20", "xxxxxxxxxxx");
    void* inputAxis = game::FindPattern("\x53\x55\x56\x57\x8B\xD9\x33\xFF\x39\xBB\xB0", "xxxxxxxxxxx");
    if (!inputKey || !inputAxis) {
        spdlog::error("TAS: input patterns not found: TAS/replays are unavailable");
        return false;
    }
    if (auto* ig = static_cast<std::uint8_t*>(game::FindPattern("\xA3\x00\x00\x00\x00\x57\x53\xFF", "x????xxx")))
        g_inputGlobal = *reinterpret_cast<std::uintptr_t*>(ig + 1);
    else
        spdlog::warn("TAS: input base pattern not found, injecting through the last real input's viewport");

    if (HMODULE crt = GetModuleHandleW(L"msvcr80.dll"))
        g_srand = reinterpret_cast<srand_t>(GetProcAddress(crt, "srand"));
    if (!g_srand) spdlog::warn("TAS: msvcr80!srand not found: random events won't be seeded (runs may desync)");

    bool ok = Hook(timeFn, reinterpret_cast<void*>(&TimeHook), reinterpret_cast<void**>(&oTime), "frame timing") &&
              Hook(inputKey, reinterpret_cast<void*>(&InputKeyHook), reinterpret_cast<void**>(&oInputKey), "InputKey") &&
              Hook(inputAxis, reinterpret_cast<void*>(&InputAxisHook), reinterpret_cast<void**>(&oInputAxis), "InputAxis") &&
              HookApi(L"kernel32.dll", "QueryPerformanceCounter", reinterpret_cast<void*>(&QpcHook), reinterpret_cast<void**>(&oQPC)) &&
              HookApi(L"kernel32.dll", "QueryPerformanceFrequency", reinterpret_cast<void*>(&QpfHook), reinterpret_cast<void**>(&oQPF)) &&
              HookApi(L"kernel32.dll", "GetTickCount", reinterpret_cast<void*>(&GetTickCountHook), reinterpret_cast<void**>(&oGetTickCount));
    if (!ok) return false;
    // Optional: not every Windows/game setup has these loaded.
    HookApi(L"kernel32.dll", "GetTickCount64", reinterpret_cast<void*>(&GetTickCount64Hook), reinterpret_cast<void**>(&oGetTickCount64));
    if (GetModuleHandleW(L"winmm.dll"))
        HookApi(L"winmm.dll", "timeGetTime", reinterpret_cast<void*>(&TimeGetTimeHook), reinterpret_cast<void**>(&oTimeGetTime));

    g_available = true;
    spdlog::info("TAS ready");
    return true;
}

bool Available() { return g_available; }

bool IsGameThread() { return GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed); }

void SetFrameHandler(FrameHandler handler) { g_frameHandler = handler; }

void UseVirtualClock(bool on)
{
    if (on == g_virtual) return;
    g_virtual = on;
    if (on) {
        *g_secondsPerCycle = 1.0 / static_cast<double>(kVirtualFrequency);
    } else {
        // Hand the engine back a clock it can continue from: real time everywhere, so its next delta is small.
        *g_secondsPerCycle = g_realSecondsPerCycle;
        const double now = RealSeconds();
        *g_current = now;
        *g_last = now;
    }
}

void SetFrameTime(double seconds, double dt)
{
    g_virtualSeconds = seconds;
    g_virtualTicks = std::llround(seconds * static_cast<double>(kVirtualFrequency));
    g_frameTicks = std::max<long long>(1, std::llround(dt * static_cast<double>(kVirtualFrequency)));
    g_calls = 0;
    *g_delta = dt;
    *g_current = seconds;
    *g_last = seconds - dt;
}

double RealSeconds()
{
    LARGE_INTEGER t{}, f{};
    (oQPC ? oQPC : &QueryPerformanceCounter)(&t);
    (oQPF ? oQPF : &QueryPerformanceFrequency)(&f);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart ? f.QuadPart : 1);
}

void SetInputFilter(InputFilter filter) { g_inputFilter = filter; }

bool Inject(const RawInput& in)
{
    void* client = nullptr;
    void* viewport = nullptr;
    if (!InputTarget(client, viewport)) return false;

    g_injecting = true;
    if (in.axis)
        oInputAxis(client, viewport, in.controllerId, in.nameIndex, in.nameNumber, in.value, in.value2, in.gamepad);
    else
        oInputKey(client, viewport, in.controllerId, in.nameIndex, in.nameNumber, in.event, in.value, in.gamepad);
    g_injecting = false;
    return true;
}

void SeedRand(std::uint32_t seed)
{
    if (g_srand) g_srand(seed);
}

void SetWorldRendering(bool on)
{
    if (auto* vp = GameViewport()) vp->bDisableWorldRendering = on ? 0 : 1;
}

std::uint32_t GameStamp()
{
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew)->FileHeader.TimeDateStamp;
}

} // namespace tas::engine
