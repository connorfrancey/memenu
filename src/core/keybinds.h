#pragma once
#include <string>

namespace keybinds {
     // A button showing "<caption>: <key>". Click to bind, right-click to clear. While armed it asks for a key; Esc
    // cancels, Backspace/Delete clears, and clicking elsewhere or closing the menu cancels as well.
    void Button(const std::string& caption, int key, float width);
    
    // Called with every fresh key press while the menu is open. True if the key was consumed by a binding.
    bool Capture(int vk);

    bool Armed();   // Something is waiting for a key
    void Cancel();
    void EndFrame();    // Once per frame, after the menu was drawn: a click elsewhere cancels

}