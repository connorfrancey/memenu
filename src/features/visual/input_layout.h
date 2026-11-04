#pragma once
#include <string>
#include <vector>

// Layouts of the input overlay (like NohBoard's keyboard files): where each key, the mouse pad and the keys per second
// go, on a grid in key widths. Saved as small JSON files in memenu\input_layouts\, so a layout is easy to edit by hand:
//
//   { "name": "Movement", "elements": [
//       { "key": "W", "x": 1, "y": 0 },                                   one key, labelled "W"
//       { "keys": ["LeftShift", "RightShift"], "label": "Shift", "x": 0, "y": 1, "w": 1.5 },   lit by either key
//       { "type": "mouse", "x": 6, "y": 1, "w": 3, "h": 2 },              the mouse movement pad
//       { "type": "kps", "x": 9, "y": 1 }                                 presses in the last second
//   ] }
//
// Key names are the engine's (Unreal Engine 3): W, SpaceBar, LeftShift, LeftControl, LeftAlt, Tab, CapsLock, one..zero,
// LeftMouseButton, RightMouseButton, MiddleMouseButton, ThumbMouseButton, ThumbMouseButton2, MouseScrollUp, ...
// x, y, w and h are in key widths (w and h default to 1); the label defaults to a short name of the first key.
namespace inputlayout {

enum class Kind { Key, Mouse, Kps };

struct Element {
    Kind kind = Kind::Key;
    std::string label;
    std::vector<std::string> keys;      // Lower case. Any of them held lights it
    float x = 0.f, y = 0.f, w = 1.f, h = 1.f;
};

struct Layout {
    std::string name;
    std::vector<Element> elements;
    float Width() const;                // Right edge of the rightmost element, in key widths
    float Height() const;
};

bool Parse(const std::string& json, Layout& out, std::string* error = nullptr);
std::string ToJson(const Layout& layout);
std::vector<Layout> Builtins();         // Movement (the default), Minimal, Mouse, Left hand
std::string ShortLabel(const std::string& key);    // "spacebar" -> "Space", "leftmousebutton" -> "LMB", "one" -> "1"

} // namespace inputlayout
