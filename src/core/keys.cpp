#include "keys.h"

#include <Windows.h>

namespace {
    bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    std::string KeyName(int vk)
    {
        switch (vk) {   // GetKeyNameText doesn't know the mouse
            case VK_LBUTTON:  return "Left Mouse";
            case VK_RBUTTON:  return "Right Mouse";
            case VK_MBUTTON:  return "Middle Mouse";
            case VK_XBUTTON1: return "Mouse 4";
            case VK_XBUTTON2: return "Mouse 5";
            default: break;
        }
        LONG lparam = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
        switch (vk)
        {
            // Extended keys need bit 24 for GetKeyNameText
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
            case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            case VK_RCONTROL: case VK_RMENU:
                lparam |= (1 << 24);
        }
        char buf[64] {};
        if (GetKeyNameTextA(lparam, buf, sizeof buf) > 0) return buf;
        return "VK " + std::to_string(vk);
    }
}

bool keys::IsModifier(int vk)
{
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
            return true;
        default:
            return false;
    }
}

int keys::HeldMods()
{
    return (Held(VK_CONTROL) ? kCtrl : 0) | (Held(VK_SHIFT) ? kShift : 0) | (Held(VK_MENU) ? kAlt : 0);
}

std::string keys::Name(int binding)
{
    if (binding == 0) return "None";
    std::string out;
    if (binding & kCtrl) out += "Ctrl+";
    if (binding & kShift) out += "Shift+";
    if (binding & kAlt) out += "Alt+";
    return out + KeyName(Vk(binding));
}

bool keys::Down(int binding)
{
    if (binding == 0 || !Held(Vk(binding))) return false;
    const int mods = Mods(binding);
    return (HeldMods() & mods) == mods;
}
