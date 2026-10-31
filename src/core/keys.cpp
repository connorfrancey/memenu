#include "keys.h"

#include <Windows.h>

std::string keys::Name(int vk)
{
    if (vk == 0) return "None";

    LONG lparam = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
    switch (vk)
    {
        // Extended keys need bit 24 for GetKeyNameText
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            lparam |= (1 << 24);
    }
    char buf[64] {};
    if (GetKeyNameTextA(lparam, buf, sizeof buf) > 0) return buf;
    return "VK " + std::to_string(vk);
}

bool keys::Down(int vk)
{
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}
