#pragma once
#include <cstdint>

// The engine-side pieces a TAS needs, found the same way mtas finds them (same byte patterns, same game build as the
// mmultiplayer SDK):
//
//   - The engine's frame timing function, which computes the frame's delta time and caps the frame rate. While a
//     session runs it is replaced: every frame gets exactly the time step the session chooses.
//   - The clocks the game thread can read on its own (QueryPerformanceCounter/Frequency, GetTickCount, timeGetTime).
//     While a session runs they return a virtual clock on the game thread, so nothing the game does can depend on how
//     fast the machine is. Other threads (rendering, audio, this menu) keep the real clock.
//   - UGameViewportClient::InputKey / InputAxis: every key, button, mouse and gamepad input goes through these. Real
//     input is handed to a filter (record, defer, or drop it) and replayed input is injected through the same calls.
//   - The CRT's rand() state of the game thread, which is seeded at the start of a run.
//
// Everything here except Init/Available is game thread only.
namespace tas::engine {

bool Init();                    // after game::Init(). False: a pattern is missing, no TAS
bool Available();

bool IsGameThread();

// ---- frame timing ---------------------------------------------------------------------------------------------------
// Called on the game thread at the start of every engine frame, before the engine ticks. Return true when the session
// has set the frame's time with SetFrameTime() (the engine's own timing is skipped), false to let the engine run it.
using FrameHandler = bool (*)();
void SetFrameHandler(FrameHandler handler);

// Switches the game thread's clocks (and the engine's seconds-per-cycle) to the virtual clock, or back to real time.
void UseVirtualClock(bool on);
// The time of this frame: `seconds` on the virtual clock, advanced by `dt`.
void SetFrameTime(double seconds, double dt);
// Real seconds from the unhooked performance counter.
double RealSeconds();

// ---- input --------------------------------------------------------------------------------------------------------------
struct RawInput {
    int nameIndex = 0, nameNumber = 0;      // the key's FName
    bool axis = false;
    int event = 0;                          // keys: EInputEvent (0 pressed, 1 released, 2 repeat, 3 double click)
    float value = 0.f;                      // keys: amount depressed. axis: delta
    float value2 = 0.f;                     // axis: delta time
    bool gamepad = false;
    int controllerId = 0;
};
// Every real input (not ones from Inject*) is offered to this filter first. Return true to keep it from the engine.
using InputFilter = bool (*)(const RawInput& input);
void SetInputFilter(InputFilter filter);

// Hands an input to the engine as if it came from the keyboard/mouse/gamepad. False: the viewport isn't known yet.
bool Inject(const RawInput& input);

// ---- misc -----------------------------------------------------------------------------------------------------------------
void SeedRand(std::uint32_t seed);          // the game thread's rand() state (msvcr80)
void SetWorldRendering(bool on);            // GameViewportClient.bDisableWorldRendering
std::uint32_t GameStamp();                  // PE timestamp of MirrorsEdge.exe

} // namespace tas::engine
