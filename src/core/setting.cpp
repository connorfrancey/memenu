#include "core/setting.h"
#include "core/keybinds.h"
#include "core/keys.h"
#include "core/module.h"
#include "core/notify.h"

#include <Windows.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ctl {

namespace {

using json = nlohmann::ordered_json;

// "%" in user-provided names would be eaten by the slider format string.
std::string Esc(const std::string& s)
{
    std::string o;
    for (char c : s) { o += c; if (c == '%') o += '%'; }
    return o;
}

// 0.6f -> 0.6 instead of 0.6000000238418579
double Round(float v) { return std::round(static_cast<double>(v) * 10000.0) / 10000.0; }

template <class T> json ToJson(const T& v) { return v; }
json ToJson(float v) { return Round(v); }
json ToJson(const std::array<float, 4>& v)
{
    json out = json::array();
    for (float c : v) out.push_back(Round(c));
    return out;
}

struct PopupStyle {   // popups use the global WindowPadding (0): give them some air
    PopupStyle()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.f, 6.f});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 4.f});
    }
    ~PopupStyle() { ImGui::PopStyleVar(2); }
};

// "<name>: <selected>" combo box. Returns the index that was clicked, or -1.
int ComboBox(const std::string& name, const std::vector<std::string>& items, int selected, float width)
{
    const bool inRange = selected >= 0 && selected < static_cast<int>(items.size());
    const std::string preview = name + ": " + (inRange ? items[static_cast<std::size_t>(selected)] : std::string());
    int clicked = -1;

    ImGui::SetNextItemWidth(width);
    PopupStyle style;
    if (ImGui::BeginCombo("##c", preview.c_str())) {
        for (int n = 0; n < static_cast<int>(items.size()); ++n) {
            ImGui::PushID(n);
            if (ImGui::Selectable(items[static_cast<std::size_t>(n)].c_str(), n == selected)) clicked = n;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return clicked;
}

// The next (slot 0) or previous (slot 1) of `n` options, wrapping around.
int Cycle(int current, int n, int slot) { return (current + (slot == 0 ? 1 : n - 1)) % n; }

} // namespace

// ---- Setting -----------------------------------------------------------------------------------------------------------
Setting::Setting(Module* owner, std::string name, std::string tooltip, Binds binds)
    : owner_(owner), name_(std::move(name)), tooltip_(std::move(tooltip)), binds_(binds)
{
    owner_->AddSetting(this);
}

void Setting::Show(float width)
{
    ImGui::PushID(this);
    Draw(width);

    // All of these refer to the control that was just drawn.
    if (ImGui::IsItemEdited()) owner_->MarkDirty();       // a scheduled module reacts at the next tick

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {   // the tooltip text is only built while hovered
        std::string tip = tooltip_;
        for (int slot = 0; slot < 2; ++slot) {
            const int k = slot == 0 ? key : keyAlt;
            if (k == 0 || !BindLabel(slot)) continue;
            if (!tip.empty()) tip += '\n';
            tip += std::string(BindLabel(slot)) + ": " + keys::Name(k);
        }
        if (Bindable()) {
            if (!tip.empty()) tip += '\n';
            tip += "Right click: set keys";
        }
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    }

    if (Bindable()) {
        PopupStyle style;
        if (ImGui::BeginPopupContextItem("##keys", ImGuiPopupFlags_MouseButtonRight)) {
            ImGui::TextDisabled("%s", name_.c_str());
            for (int slot = 0; slot < 2; ++slot)
                if (const char* label = BindLabel(slot))
                    keybinds::Button(std::string(label) + " key", slot == 0 ? key : keyAlt, 200.f);
            ImGui::TextDisabled("Right click a key to clear it");
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
}

void Setting::Press(int slot)
{
    const std::string value = Fire(slot);
    owner_->MarkDirty();
    if (!value.empty()) notify::Push(owner_->Name() + " - " + name_ + ": " + value);
}

// ---- Value -------------------------------------------------------------------------------------------------------------
template <class T>
void Value<T>::Save(json& out) const { out = ToJson(value_); }

template <class T>
void Value<T>::Load(const json& in)
{
    try {
        Set(in.get<T>());
    } catch (const json::exception&) {
        // wrong type in the file: keep the current value
    }
}

template class Value<bool>;
template class Value<int>;
template class Value<float>;
template class Value<std::array<float, 4>>;

// ---- Checkbox --------------------------------------------------------------------------------------------------------
Checkbox::Checkbox(Module* owner, std::string name, bool value, std::string tooltip)
    : Value(owner, std::move(name), value, std::move(tooltip), {"Toggle", nullptr}) {}

void Checkbox::Draw(float)
{
    ImGui::Checkbox(Name().c_str(), &value_);
}

std::string Checkbox::Fire(int)
{
    value_ = !value_;
    return value_ ? "ON" : "OFF";
}

// ---- Slider / SliderInt ------------------------------------------------------------------------------------------------
template <class T>
BasicSlider<T>::BasicSlider(Module* owner, std::string name, T value, T min, T max, std::string format, T step,
                            std::string tooltip)
    : Value<T>(owner, std::move(name), value, std::move(tooltip), {"Increase", "Decrease"}),
      min_(min), max_(max), format_(std::move(format))
{
    if constexpr (std::is_integral_v<T>) step_ = std::max<T>(1, step);
    else                                 step_ = step > 0 ? step : (max - min) / 20;
    this->Set(value);
}

template <class T>
T BasicSlider<T>::Clamp(T v) const { return std::clamp(v, min_, max_); }

template <class T>
void BasicSlider<T>::Draw(float width)
{
    constexpr ImGuiDataType type = std::is_integral_v<T> ? ImGuiDataType_S32 : ImGuiDataType_Float;
    const std::string fmt = Esc(this->Name()) + ": " + format_;
    ImGui::SetNextItemWidth(width);
    ImGui::SliderScalar("##v", type, &this->value_, &min_, &max_, fmt.c_str());
}

template <class T>
std::string BasicSlider<T>::Fire(int slot)
{
    this->Set(this->value_ + (slot == 0 ? step_ : -step_));
    char buf[64];
    if constexpr (std::is_integral_v<T>) std::snprintf(buf, sizeof buf, format_.c_str(), this->value_);
    else                                 std::snprintf(buf, sizeof buf, format_.c_str(), static_cast<double>(this->value_));
    return buf;
}

template class BasicSlider<float>;
template class BasicSlider<int>;

// ---- Combo -----------------------------------------------------------------------------------------------------------
Combo::Combo(Module* owner, std::string name, std::vector<std::string> items, int value, std::string tooltip)
    : Value(owner, std::move(name), value, std::move(tooltip), {"Next", "Previous"}), items_(std::move(items))
{
    Set(value);
}

int Combo::Clamp(int index) const
{
    return items_.empty() ? 0 : std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
}

const std::string& Combo::Selected() const
{
    static const std::string none;
    return value_ >= 0 && value_ < static_cast<int>(items_.size()) ? items_[static_cast<std::size_t>(value_)] : none;
}

void Combo::Draw(float width)
{
    if (const int n = ComboBox(Name(), items_, value_, width); n >= 0) value_ = n;
}

std::string Combo::Fire(int slot)
{
    if (items_.empty()) return {};
    value_ = Cycle(value_, static_cast<int>(items_.size()), slot);
    return Selected();
}

// ---- DynamicCombo ----------------------------------------------------------------------------------------------------
DynamicCombo::DynamicCombo(Module* owner, std::string name, std::function<std::vector<std::string>()> items,
                           std::function<int()> get, std::function<void(int)> set, std::string tooltip)
    : Setting(owner, std::move(name), std::move(tooltip), {"Next", "Previous"}),
      items_(std::move(items)), get_(std::move(get)), set_(std::move(set)) {}

void DynamicCombo::Draw(float width)
{
    const std::vector<std::string> items = items_ ? items_() : std::vector<std::string>{};
    if (const int n = ComboBox(Name(), items, get_ ? get_() : -1, width); n >= 0 && set_) set_(n);
}

std::string DynamicCombo::Fire(int slot)
{
    const std::vector<std::string> items = items_ ? items_() : std::vector<std::string>{};
    if (items.empty() || !set_) return {};
    const int n = static_cast<int>(items.size());
    const int next = Cycle(get_ ? std::clamp(get_(), 0, n - 1) : 0, n, slot);
    set_(next);
    return items[static_cast<std::size_t>(next)];
}

// ---- ColorPick -------------------------------------------------------------------------------------------------------
ColorPick::ColorPick(Module* owner, std::string name, std::array<float, 4> value, std::string tooltip)
    : Value(owner, std::move(name), value, std::move(tooltip), {"Next hue", "Previous hue"})
{
    Set(value);
}

std::array<float, 4> ColorPick::Clamp(std::array<float, 4> v) const
{
    for (float& c : v) c = std::clamp(c, 0.f, 1.f);
    return v;
}

unsigned int ColorPick::Packed() const
{
    return ImGui::ColorConvertFloat4ToU32({value_[0], value_[1], value_[2], value_[3]});
}

void ColorPick::Draw(float)
{
    PopupStyle style;
    ImGui::ColorEdit4("##col", value_.data(),
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    ImGui::SameLine(0.f, 8.f);
    ImGui::TextUnformatted(Name().c_str());
}

std::string ColorPick::Fire(int slot)
{
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(value_[0], value_[1], value_[2], h, s, v);
    h = std::fmod(h + (slot == 0 ? 1.f : -1.f) / 12.f + 1.f, 1.f);
    ImGui::ColorConvertHSVtoRGB(h, s, v, value_[0], value_[1], value_[2]);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d deg", static_cast<int>(h * 360.f + 0.5f) % 360);
    return buf;
}

// ---- KeyHold ---------------------------------------------------------------------------------------------------------
KeyHold::KeyHold(Module* owner, std::string name, int vk, std::string tooltip)
    : Value(owner, std::move(name), vk, std::move(tooltip)) {}

bool KeyHold::Down() const { return keys::Down(value_); }

void KeyHold::Draw(float width) { keybinds::Button(Name(), value_, width); }

// ---- Button ----------------------------------------------------------------------------------------------------------
Button::Button(Module* owner, std::string name, std::function<void()> onClick, std::string tooltip)
    : Setting(owner, std::move(name), std::move(tooltip), {"Press", nullptr}), onClick_(std::move(onClick)) {}

void Button::Draw(float width)
{
    if (ImGui::Button(Name().c_str(), {width, 0.f}) && onClick_) onClick_();
}

std::string Button::Fire(int)
{
    if (onClick_) onClick_();
    return {};
}

// ---- Label / Separator / Custom --------------------------------------------------------------------------------------
Label::Label(Module* owner, std::string text) : Setting(owner, std::move(text)) {}

void Label::Draw(float width)
{
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextDisabled("%s", Name().c_str());
    ImGui::PopTextWrapPos();
}

Separator::Separator(Module* owner, std::string title) : Setting(owner, std::move(title)) {}

void Separator::Draw(float)
{
    if (Name().empty()) ImGui::Separator();
    else                ImGui::SeparatorText(Name().c_str());
}

Custom::Custom(Module* owner, std::function<void(float)> draw) : Setting(owner, {}), draw_(std::move(draw)) {}

void Custom::Draw(float width)
{
    if (draw_) draw_(width);
}

} // namespace ctl
