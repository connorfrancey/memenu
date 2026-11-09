#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

// What the game is getting from the keyboard and mouse, for an input display: the keys held right now, how often each
// was pressed, the recent mouse movement. Fed by tas::engine's input monitor, so it is the input the engine really gets:
// your own while you play (not while the menu is open: the game doesn't get it then), and a TAS's while it records or
// plays a macro back.
//
// Key names are the engine's, lower case: "w", "spacebar", "leftshift", "leftmousebutton", "mousescrollup", "one".
namespace tas::input {

bool Install();                 // after tas::Init(). False: no engine hooks (the display falls back to the keyboard)
bool Installed();

struct Key {
    bool held = false;
    double down = -1.0;         // When it last went down (Now() seconds), < 0: never
    double up = -1.0;           // When it last came up
    std::uint32_t presses = 0;
};

struct Snapshot {
    std::unordered_map<std::string, Key> keys;
    float mouseX = 0.f, mouseY = 0.f;   // Movement in the last `window` seconds (the engine's units: mouse counts)
    int kps = 0;                        // Key and button presses in the last second
    double now = 0.0;
};

// `window`: how far back the mouse movement is added up.
Snapshot Get(double window = 0.05);
void ResetCounts();
double Now();                   // The clock the times above are on (real seconds)

} // namespace tas::input
