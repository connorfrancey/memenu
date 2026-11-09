#pragma once
#include <string>
#include <vector>

class Module;
namespace ctl { class Setting; }

// Every key the menu can be told to use, in one list: module toggle keys / hotkeys, the keys given to controls
// (right click a control), and the key settings modules poll themselves (ctl::KeyHold: TAS keys, flight keys, ...).
// The hotkey list shows them all and the bind buttons use it to warn about a key that does something else already.
// Menu (render) thread.
namespace bindings {
    enum class Kind {
        Toggle,     // Module::key
        Control,    // ctl::Setting::key / keyAlt
        Key,        // ctl::KeyHold's value
    };

    struct Slot {
        Kind kind = Kind::Toggle;
        Module* module = nullptr;
        ctl::Setting* setting = nullptr;    // Control and Key
        int* binding = nullptr;             // What the bind button edits
        std::string what;                   // "Toggle", "Hotkey", "Speed: Increase", "Pause / resume"

        std::string Describe() const;       // "No Clip: Toggle"
    };

    const std::vector<Slot>& All();    // Cached: rebuilt only when modules are added

    // Two bindings that the same press fires: the same key with the same modifiers.
    bool Clash(int a, int b);
    // The other slots bound so that they clash with `binding` ("No Clip: Toggle"), leaving out the slot `self`.
    std::vector<std::string> ClashesWith(int binding, const int* self);
    // How many bound slots clash with at least one other.
    int ClashCount();
}
