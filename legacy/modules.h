#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Module;

// One control inside a module's expanded panel.
struct Setting {
    enum class Type { Bool, Float, Int, Combo, Color, Key, Button, Label, Separator, Custom };

    Type type = Type::Bool;
    std::string name;
    std::string tooltip;

    bool  b = false;                          // Bool
    float f = 0.f, fmin = 0.f, fmax = 1.f;    // Float
    std::string format = "%.2f";              // Float display format (value only, e.g. "%.1fx")
    int   i = 0, imin = 0, imax = 1;          // Int value | Combo selected index | Key (VK code, 0 = none)
    std::vector<std::string> options;         // Combo
    std::array<float, 4> color{1.f, 1.f, 1.f, 1.f};   // Color (RGBA)

    bool persist = true;                      // Saved to / loaded from the config file

    // Combo whose options/selection live elsewhere (e.g. the theme list). When set, `options` and
    // `i` are ignored and the control is never saved.
    std::function<std::vector<std::string>()> optionsFn;
    std::function<int()>                      getIndex;
    std::function<void(int)>                  setIndex;

    std::function<void(Module&)> onClick;     // Button click | Key press (fires while module is on)
    std::function<void(Module&)> onChange;    // Any value change made through the GUI
    std::function<bool(Module&)> visibleIf;   // Hide the control when this returns false

    // Custom: draws arbitrary ImGui context (e.g. a dynamic list of buttons). `width` is the usable width.
    std::function<void(Module&, float width)> draw;
};

struct Module {
    std::string name, description;
    bool isAction = false;          // True: row is a button (runs onClick) instead of a toggle
    bool enabledByDefault = false;
    bool enabled = false;
    int  key = 0;                   // VK code of the module hotkey, 0 = unbound
    std::vector<Setting> settings;

    std::function<void(Module&)> onEnable;
    std::function<void(Module&)> onDisable;
    std::function<void(Module&)> onTick;     // Every frame while enabled (game-side logic)
    std::function<void(Module&)> onRender;   // Every frame while enabled, inside the ImGui frame (HUD)
    std::function<void(Module&)> onClick;    // Action modules

    // GUI state
    bool expanded = false;
    float anim = 0.f, hover = 0.f;

    void SetEnabled(bool v)
    {
        if (isAction || v == enabled) return;
        enabled = v;
        const auto& cb = v ? onEnable : onDisable;
        if (cb) cb(*this);
    }

    void Toggle()
    {
        if (isAction) { if (onClick) onClick(*this); }
        else          SetEnabled(!enabled);
    }

    // ---- Setting access (by name) ----
    Setting* Find(std::string_view n)
    {
        for (auto& s : settings) if (s.name == n) return &s;
        return nullptr;
    }
    bool  GetBool (std::string_view n) { auto* s = Find(n); return s && s->b; }
    float GetFloat(std::string_view n) { auto* s = Find(n); return s ? s->f : 0.f; }
    int   GetInt  (std::string_view n) { auto* s = Find(n); return s ? s->i : 0; }
    int   GetCombo(std::string_view n) { auto* s = Find(n); return s ? s->i : 0; }
    int   GetKey  (std::string_view n) { auto* s = Find(n); return s ? s->i : 0; }
    std::array<float, 4> GetColor(std::string_view n)
    { auto* s = Find(n); return s ? s->color : std::array<float, 4>{1.f, 1.f, 1.f, 1.f}; }
    std::string GetComboName(std::string_view n)
    {
        auto* s = Find(n);
        if (!s || s->i < 0 || s->i >= static_cast<int>(s->options.size())) return {};
        return s->options[static_cast<size_t>(s->i)];
    }
    bool KeyDown(std::string_view n);   // Is the key bound to Key setting `n` currently held?
};

struct Tab {
    std::string name;
    float width = 200.f;
    std::vector<std::unique_ptr<Module>> modules;
};

namespace modules {
    std::vector<std::unique_ptr<Tab>>& Tabs();
    Tab&    AddTab(std::string name);          // Returns the existing tab if the name is taken
    Module* Find(std::string_view name);       // First module with this name, any tab

    template <class F>
    void ForEachModule(F&& f) { for (auto& t : Tabs()) for (auto& m : t->modules) f(*t, *m); }

    void Init();            // Defined in definitions.cpp: builds the whole menu
    void ApplyDefaults();   // Turns on modules flagged enabledByDefault (call after Init, before config load)
    void Tick();            // onTick of enabled modules
    void Render();          // onRender of enabled modules
    void OnKey(int vk);     // Module hotkeys + Key-setting callbacks
    void DisableAll();      // Call on unload so modules can undo their changes
}
