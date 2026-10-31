#pragma once
#include <string>

// Windows virtual-key helpers shared by the menu and the modules.
namespace keys {
    std::string Name(int vk);   // "F5", "Left Shift", ... ("None" for 0)
    bool Down(int vk);          // Is the key physically held right now? (false for 0)
}