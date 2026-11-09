#pragma once
// Developer mode: shows the developer-only modules (Test: the in-game self-tests). It is the "Developer Mode" row in the
// Misc tab, so the menu stores it in MedgeMenu.json like any module and it stays on across starts until switched off.
//
// A developer build (`memenu --dev build`, CMake option MEMENU_DEV_MODE) turns it on at every start (ApplyBuild); a
// normal build keeps what was stored.
#ifndef MEMENU_DEV_MODE
#define MEMENU_DEV_MODE 0
#endif

namespace devmode {
    bool Enabled();         // Any thread
    void ApplyBuild();      // After config::Load: a developer build switches it on (and the config stores that)

    // Modules a run of the self-test never switches (switching them would hide the test's own window, or turn the
    // developer tools off under it)
    constexpr const char* kModuleName = "Developer Mode";
}
