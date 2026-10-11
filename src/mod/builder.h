#pragma once
#include "modules.h"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <cassert>

// Fluent builder for tabs, modules and controls:
//
//   Builder ui;
//   ui.tab("Player")
//       .module("No Clip", "Fly through geometry")
//           .hotkey(VK_F5)
//           .slider("Speed", 1.f, 0.1f, 10.f, "%.1fx")
//           .combo("Mode", {"Fly", "Ghost"})
//           .onTick([](Module& m) { ... })
//       .action("Teleport to spawn", "Back to the start", [](Module&) { ... })
//   .tab("Visual")
//       .module("Crosshair") ...
//
// Control calls attach to the most recent module(); .tooltip/.visibleIf/.onChange attach to the
// most recent control; .onTick/.onRender/... attach to the most recent module.
class Builder {
public:
    // ---- Structure -------------------------------------------------------------------------
    Builder& tab(std::string name)
    {
        tab_ = &modules::AddTab(std::move(name));
        mod_ = nullptr;
        return *this;
    }

    Builder& module(std::string name, std::string description = {})
    {
        assert(tab_ && "call tab() before module()");
        auto m = std::make_unique<Module>();
        m->name = std::move(name);
        m->description = std::move(description);
        mod_ = m.get();
        tab_->modules.push_back(std::move(m));
        return *this;
    }

    // A row that behaves like a button: click (or its hotkey) runs `fn`.
    Builder& action(std::string name, std::string description, std::function<void(Module&)> fn)
    {
        module(std::move(name), std::move(description));
        mod_->isAction = true;
        mod_->onClick = std::move(fn);
        return *this;
    }

    // ---- Module Options --------------------------------------------------------------------
    Builder& enabledByDefault(bool v = true) { cur().enabledByDefault = v; return *this; }
    Builder& hotkey(int vk)                  { cur().key = vk; return *this; }
    Builder& onEnable (std::function<void(Module&)> f) { cur().onEnable  = std::move(f); return *this; }
    Builder& onDisable(std::function<void(Module&)> f) { cur().onDisable = std::move(f); return *this; }
    Builder& onTick   (std::function<void(Module&)> f) { cur().onTick    = std::move(f); return *this; }
    Builder& onRender (std::function<void(Module&)> f) { cur().onRender  = std::move(f); return *this; }

    // ---- Controls --------------------------------------------------------------------------
    Builder& checkbox(std::string name, bool def = false)
    {
        auto& s = add(Setting::Type::Bool, std::move(name));
        s.b = def;
        return *this;
    }
    // `fmt` is the value format, e.g. "%.1fx" or "%.0f deg".
    Builder& slider(std::string name, float def, float lo, float hi, std::string fmt = "%.2f")
    {
        if (lo > hi) throw std::invalid_argument("slider: minimum exceeds maximum");
        if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(def)) throw std::invalid_argument("slider: values must be finite");
        if (def < lo || def > hi) throw std::invalid_argument("slider: default is out of range");

        auto& s = add(Setting::Type::Float, std::move(name));
        s.f = def; s.fmin = lo; s.fmax = hi; s.format = std::move(fmt);
        return *this;
    }
    Builder& sliderInt(std::string name, int def, int lo, int hi)
    {
        if (lo > hi) throw std::invalid_argument("slider: minimum exceeds maximum");
        if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(def)) throw std::invalid_argument("slider: values must be finite");
        if (def < lo || def > hi) throw std::invalid_argument("slider: default is out of range");
        auto& s = add(Setting::Type::Int, std::move(name));
        s.i = def; s.imin = lo; s.imax = hi;
        return *this;
    }
    Builder& combo(std::string name, std::vector<std::string> options, int def = 0)
    {
        auto& s = add(Setting::Type::Combo, std::move(name));
        s.options = std::move(options); s.i = def;
        return *this;
    }
    // Combo driven by callbacks: options are fetched every frame, selection read/written through
    // get/set. Not saved (the owner of the state is responsible for that).
    Builder& dynamicCombo(std::string name, std::function<std::vector<std::string>()> options,
                          std::function<int()> get, std::function<void(int)> set)
    {
        auto& s = add(Setting::Type::Combo, std::move(name));
        s.optionsFn = std::move(options); s.getIndex = std::move(get); s.setIndex = std::move(set);
        s.persist = false;
        return *this;
    }
    Builder& color(std::string name, std::array<float, 4> def = {1.f, 1.f, 1.f, 1.f})
    {
        auto& s = add(Setting::Type::Color, std::move(name));
        s.color = def;
        return *this;
    }
    // Extra key binding. `onPress` runs when the key is pressed while the module is on
    // (or always, for action modules). Pass nullptr and poll with Module::KeyDown() instead.
    Builder& keybind(std::string name, int defaultVk = 0, std::function<void(Module&)> onPress = nullptr)
    {
        auto& s = add(Setting::Type::Key, std::move(name));
        s.i = defaultVk; s.onClick = std::move(onPress);
        return *this;
    }
    Builder& button(std::string name, std::function<void(Module&)> onClick)
    {
        auto& s = add(Setting::Type::Button, std::move(name));
        s.onClick = std::move(onClick);
        return *this;
    }
    Builder& label(std::string text)               { add(Setting::Type::Label, std::move(text)); return *this; }
    Builder& separator(std::string title = {})     { add(Setting::Type::Separator, std::move(title)); return *this; }

    // ---- Modifiers for the most recent control ---------------------------------------------
    Builder& tooltip(std::string text)                      { last().tooltip   = std::move(text); return *this; }
    Builder& visibleIf(std::function<bool(Module&)> pred)   { last().visibleIf = std::move(pred); return *this; }
    Builder& onChange(std::function<void(Module&)> f)       { last().onChange  = std::move(f);    return *this; }
    Builder& noSave()                                       { last().persist   = false;           return *this; }

private:
    Module& cur()  { assert(mod_ && "call module() first"); return *mod_; }
    Setting& last(){ assert(mod_ && !mod_->settings.empty() && "add a control first"); return mod_->settings.back(); }
    Setting& add(Setting::Type t, std::string name)
    {
        Setting s;
        s.type = t;
        s.name = std::move(name);
        cur().settings.push_back(std::move(s));
        return mod_->settings.back();
    }

    Tab*    tab_ = nullptr;
    Module* mod_ = nullptr;
};
