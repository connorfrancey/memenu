#include "features/visual/input_layout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace inputlayout {
namespace {

using json = nlohmann::ordered_json;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Element Key(const char* key, float x, float y, float w = 1.f, float h = 1.f, const char* label = nullptr)
{
    Element e;
    e.keys = {Lower(key)};
    e.label = label ? label : ShortLabel(e.keys.front());
    e.x = x; e.y = y; e.w = w; e.h = h;
    return e;
}

Element Keys(std::vector<std::string> keys, const char* label, float x, float y, float w = 1.f, float h = 1.f)
{
    Element e;
    for (auto& k : keys) e.keys.push_back(Lower(k));
    e.label = label;
    e.x = x; e.y = y; e.w = w; e.h = h;
    return e;
}

Element Special(Kind kind, float x, float y, float w, float h)
{
    Element e;
    e.kind = kind;
    e.label = kind == Kind::Kps ? "KPS" : "";
    e.x = x; e.y = y; e.w = w; e.h = h;
    return e;
}

// The mouse: three buttons, the wheel, the movement pad and the presses per second, from column `x`.
void AddMouse(Layout& l, float x, bool kps = true)
{
    l.elements.push_back(Key("LeftMouseButton", x, 0.f));
    l.elements.push_back(Key("MiddleMouseButton", x + 1.f, 0.f, 1.f, 1.f, "M"));
    l.elements.push_back(Key("RightMouseButton", x + 2.f, 0.f));
    l.elements.push_back(Key("MouseScrollUp", x + 3.f, 0.f, 0.6f, 0.5f));
    l.elements.push_back(Key("MouseScrollDown", x + 3.f, 0.5f, 0.6f, 0.5f));
    l.elements.push_back(Special(Kind::Mouse, x, 1.f, 3.f, 2.f));
    if (kps) l.elements.push_back(Special(Kind::Kps, x + 3.f, 1.f, 0.6f * 2.f, 1.f));
}

} // namespace

float Layout::Width() const
{
    float w = 0.f;
    for (const Element& e : elements) w = std::max(w, e.x + e.w);
    return w;
}

float Layout::Height() const
{
    float h = 0.f;
    for (const Element& e : elements) h = std::max(h, e.y + e.h);
    return h;
}

std::string ShortLabel(const std::string& keyName)
{
    const std::string k = Lower(keyName);
    static const struct { const char* key; const char* label; } kNames[] = {
        {"spacebar", "Space"}, {"leftshift", "Shift"}, {"rightshift", "Shift"}, {"leftcontrol", "Ctrl"},
        {"rightcontrol", "Ctrl"}, {"leftalt", "Alt"}, {"rightalt", "Alt"}, {"tab", "Tab"}, {"capslock", "Caps"},
        {"escape", "Esc"}, {"enter", "Enter"}, {"backspace", "Bksp"}, {"leftmousebutton", "LMB"},
        {"rightmousebutton", "RMB"}, {"middlemousebutton", "MMB"}, {"thumbmousebutton", "M4"},
        {"thumbmousebutton2", "M5"}, {"mousescrollup", "^"}, {"mousescrolldown", "v"}, {"up", "Up"},
        {"down", "Down"}, {"left", "Left"}, {"right", "Right"}, {"one", "1"}, {"two", "2"}, {"three", "3"},
        {"four", "4"}, {"five", "5"}, {"six", "6"}, {"seven", "7"}, {"eight", "8"}, {"nine", "9"}, {"zero", "0"},
    };
    for (const auto& n : kNames)
        if (k == n.key) return n.label;
    if (k.size() == 1) return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(k[0]))));
    std::string out = keyName;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::vector<Layout> Builtins()
{
    std::vector<Layout> out;

    // The keys a run uses, and the mouse
    Layout move;
    move.name = "Movement";
    move.elements = {
        Key("Tab", 0.f, 0.f, 1.5f), Key("Q", 1.5f, 0.f), Key("W", 2.5f, 0.f), Key("E", 3.5f, 0.f), Key("R", 4.5f, 0.f),
        Keys({"LeftShift", "RightShift"}, "Shift", 0.f, 1.f, 1.5f), Key("A", 1.5f, 1.f), Key("S", 2.5f, 1.f),
        Key("D", 3.5f, 1.f), Key("F", 4.5f, 1.f),
        Keys({"LeftControl", "RightControl"}, "Ctrl", 0.f, 2.f, 1.5f), Key("SpaceBar", 1.5f, 2.f, 4.f),
    };
    AddMouse(move, 6.f);
    out.push_back(std::move(move));

    // WASD, space and the two buttons
    Layout minimal;
    minimal.name = "Minimal";
    minimal.elements = {
        Key("W", 1.f, 0.f), Key("A", 0.f, 1.f), Key("S", 1.f, 1.f), Key("D", 2.f, 1.f), Key("SpaceBar", 0.f, 2.f, 3.f),
        Key("LeftMouseButton", 3.5f, 0.f, 1.f, 1.5f), Key("RightMouseButton", 4.5f, 0.f, 1.f, 1.5f),
    };
    out.push_back(std::move(minimal));

    Layout mouse;
    mouse.name = "Mouse";
    AddMouse(mouse, 0.f);
    out.push_back(std::move(mouse));

    // The whole left half of the keyboard
    Layout left;
    left.name = "Left hand";
    left.elements = {Key("Escape", 0.f, 0.f, 1.f)};
    const char* digits[] = {"one", "two", "three", "four", "five"};
    for (int i = 0; i < 5; ++i) left.elements.push_back(Key(digits[i], 1.f + static_cast<float>(i), 0.f));
    left.elements.push_back(Key("Tab", 0.f, 1.f, 1.5f));
    const char* top[] = {"Q", "W", "E", "R", "T"};
    for (int i = 0; i < 5; ++i) left.elements.push_back(Key(top[i], 1.5f + static_cast<float>(i), 1.f));
    left.elements.push_back(Key("CapsLock", 0.f, 2.f, 1.75f));
    const char* home[] = {"A", "S", "D", "F", "G"};
    for (int i = 0; i < 5; ++i) left.elements.push_back(Key(home[i], 1.75f + static_cast<float>(i), 2.f));
    left.elements.push_back(Keys({"LeftShift"}, "Shift", 0.f, 3.f, 2.25f));
    const char* bottom[] = {"Z", "X", "C", "V", "B"};
    for (int i = 0; i < 5; ++i) left.elements.push_back(Key(bottom[i], 2.25f + static_cast<float>(i), 3.f));
    left.elements.push_back(Keys({"LeftControl"}, "Ctrl", 0.f, 4.f, 1.5f));
    left.elements.push_back(Keys({"LeftAlt"}, "Alt", 1.5f, 4.f, 1.25f));
    left.elements.push_back(Key("SpaceBar", 2.75f, 4.f, 4.5f));
    AddMouse(left, 8.f);
    out.push_back(std::move(left));
    return out;
}

bool Parse(const std::string& text, Layout& out, std::string* error)
{
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("not JSON");
    if (!j.contains("elements") || !j["elements"].is_array()) return fail("no \"elements\" list");
    Layout l;
    l.name = j.value("name", std::string());
    for (const json& e : j["elements"]) {
        if (!e.is_object()) return fail("an element isn't an object");
        Element el;
        const std::string type = Lower(e.value("type", std::string("key")));
        if (type == "mouse") el.kind = Kind::Mouse;
        else if (type == "kps") el.kind = Kind::Kps;
        else if (type != "key") return fail("unknown type \"" + type + "\"");
        if (e.contains("key") && e["key"].is_string()) el.keys.push_back(Lower(e["key"].get<std::string>()));
        if (e.contains("keys") && e["keys"].is_array())
            for (const json& k : e["keys"])
                if (k.is_string()) el.keys.push_back(Lower(k.get<std::string>()));
        if (el.kind == Kind::Key && el.keys.empty()) return fail("a key element needs \"key\" or \"keys\"");
        el.x = e.value("x", 0.f);
        el.y = e.value("y", 0.f);
        el.w = std::max(0.1f, e.value("w", el.kind == Kind::Mouse ? 2.f : 1.f));
        el.h = std::max(0.1f, e.value("h", el.kind == Kind::Mouse ? 2.f : 1.f));
        el.label = e.value("label", el.kind == Kind::Kps ? std::string("KPS")
                                    : el.keys.empty() ? std::string() : ShortLabel(el.keys.front()));
        l.elements.push_back(std::move(el));
    }
    if (l.elements.empty()) return fail("no elements");
    out = std::move(l);
    return true;
}

std::string ToJson(const Layout& layout)
{
    json elements = json::array();
    for (const Element& e : layout.elements) {
        json o;
        if (e.kind != Kind::Key) o["type"] = e.kind == Kind::Mouse ? "mouse" : "kps";
        if (e.keys.size() == 1) o["key"] = e.keys.front();
        else if (e.keys.size() > 1) o["keys"] = e.keys;
        if (e.kind == Kind::Key && e.label != ShortLabel(e.keys.front())) o["label"] = e.label;
        o["x"] = e.x;
        o["y"] = e.y;
        if (e.w != 1.f) o["w"] = e.w;
        if (e.h != 1.f) o["h"] = e.h;
        elements.push_back(std::move(o));
    }
    json j;
    j["name"] = layout.name;
    j["elements"] = std::move(elements);
    // One element per line: easy to read and edit
    std::string s = "{\n  \"name\": " + json(layout.name).dump() + ",\n  \"elements\": [\n";
    for (std::size_t i = 0; i < j["elements"].size(); ++i)
        s += "    " + j["elements"][i].dump() + (i + 1 < j["elements"].size() ? ",\n" : "\n");
    s += "  ]\n}\n";
    return s;
}

} // namespace inputlayout
