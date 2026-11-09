#pragma once
#include <string>
#include <vector>

namespace keybinds {
    // A button showing "<caption>: <key>". Click to bind, right-click to clear. While armed it asks for a key or a
    // combo (hold Ctrl / Shift / Alt, then press the key; a modifier pressed and released alone is bound on its own,
    // and mouse buttons 3-5 work too). Esc cancels, Backspace/Delete clears, and clicking elsewhere or closing the menu
    // cancels as well. A key that something else uses already is drawn orange, with what in the tooltip.
    void Button(const std::string& caption, int& key, float width);

    // Called with every fresh key press while the menu is open. True if the key was consumed by a binding.
    bool Capture(int vk);

    bool Armed();   // Something is waiting for a key
    void Cancel();
    void EndFrame();    // Once per frame, after the menu was drawn: a click elsewhere cancels

    // Middle mouse, Mouse 4 and Mouse 5 have no key message the menu sees: polled once per frame. The buttons pressed
    // since the last call (none unless `focused`: the game has the focus).
    std::vector<int> PollMouseButtons(bool focused);
}
