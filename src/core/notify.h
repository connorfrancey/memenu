#pragma once
#include <string>

// Short on-screen messages ("No Clip: ON", "Speed: 1200"), shown when something is changed with a key.
namespace notify {
    void Push(std::string text);    // Any thread
    void Draw();                    // Render thread, inside the ImGui frame
}