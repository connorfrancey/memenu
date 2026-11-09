#pragma once
#include <string>

// Windows virtual-key helpers shared by the menu and the modules.
//
// A binding is an int: the virtual-key code in the low 16 bits, and the modifiers it needs (Ctrl, Shift, Alt) above
// them. A plain key is just its virtual-key code, so configs written before chords existed load unchanged. Mouse
// buttons are keys too (VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2).
//
// A binding fires when its key is pressed while its modifiers are held. Extra modifiers don't stop it (F5 still works
// while Shift is held to slide), but when two bindings of the same key fire, the one asking for more modifiers wins:
// Ctrl+F5 doesn't also do what F5 does (see Specificity()).
namespace keys {
    constexpr int kCtrl = 1 << 16;
    constexpr int kShift = 1 << 17;
    constexpr int kAlt = 1 << 18;
    constexpr int kMods = kCtrl | kShift | kAlt;

    constexpr int Vk(int binding) { return binding & 0xFFFF; }
    constexpr int Mods(int binding) { return binding & kMods; }
    constexpr int Specificity(int binding)      // How many modifiers it needs
    {
        return ((binding & kCtrl) ? 1 : 0) + ((binding & kShift) ? 1 : 0) + ((binding & kAlt) ? 1 : 0);
    }

    bool IsModifier(int vk);        // Shift / Ctrl / Alt, either side
    int HeldMods();                 // The modifiers held right now (kCtrl | ...)

    // A press of `vk` with `mods` held fires `binding`: same key, and every modifier it needs is held.
    constexpr bool Fires(int binding, int vk, int mods)
    {
        return binding != 0 && Vk(binding) == vk && (Mods(binding) & mods) == Mods(binding);
    }

    std::string Name(int binding);  // "F5", "Ctrl+Shift+F5", "Mouse 4", ... ("None" for 0)
    bool Down(int binding);         // The key is held with the binding's modifiers (false for 0)
}
