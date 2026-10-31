#include "keybinds.h"
#include "keys.h"

#include <Windows.h>
#include <imgui.h>

namespace {
    int* g_target = nullptr;        // The key variable waiting for a press
    bool g_buttonHit = false;       // A click this frame landed on a bind button
}

void keybinds::Button(const std::string &caption, int key, float width) {
    const bool armed = g_target == &key;
    
}
