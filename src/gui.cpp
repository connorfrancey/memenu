#include "gui.h"
#include "modules.h"
#include "theme.h"

#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <string>
#include <imgui.h>

namespace
{
    // What is waiting for a key press: a module hotkey (setting == nullptr) or a Key setting.
    struct Binding { Module* mod = nullptr; Setting* setting = nullptr; } g_bind;
    bool g_keyButtonHit = false; // A click this frame landed on a bind button

    bool IsBinding(Module* m, Setting* s) { return g_bind.mod == m && g_bind.setting == s; }

    ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }
    ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
    void Approach(float& v, float target, float dt) { v += (target - v) * std::min(1.f, dt * 14.f); }

    // "%" in user-provided names would be eaten by the slider format string.
    std::string Esc(const std::string& s)
    {
        std::string o;
        for (char c : s) { o += c; if (c == '%') o += '%'; }
        return o;
    }

    std::string KeyName(int vk)
    {
        LONG lparam = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
        switch (vk)
        {
            // xtended keys need bit 24 for GetKeyNameText
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_END:
            case VK_HOME:
            case VK_INSERT:
            case VK_DELETE:
            case VK_DIVIDE:
            case VK_NUMLOCK:
                lparam |= (1 << 24);
        }
        char buf[64]{};
        if (GetKeyNameTextA(lparam, buf, sizeof buf) > 0) return buf;
        return "VK " + std::to_string(vk);
    }

    // Click to bind, right-click to clear. While waiting: Esc / click elsewhere / closing the menu cancels.
    void KeyButton(const std::string& caption, int& key, Module& m, Setting* s, float w)
    {
        const bool binding = IsBinding(&m, s);
        const std::string text = binding
                                     ? std::string("Press key... Esc=cancel")
                                     : caption + ": " + (key ? KeyName(key) : std::string("None"));
        if (ImGui::Button((text + "##key").c_str(), {w, 0.f}))
            g_bind = binding ? Binding{} : Binding{&m, s};
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
            g_keyButtonHit = true;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            key = 0;
            if (binding) g_bind = {};
        }
    }

    void DrawControl(Module& m, Setting& s, float itemW)
    {
        bool changed = false;
        const std::string label = Esc(s.name);

        switch (s.type)
        {
            case Setting::Type::Bool:
                changed = ImGui::Checkbox(s.name.c_str(), &s.b);
                break;

            case Setting::Type::Float:
                {
                    ImGui::SetNextItemWidth(itemW);
                    const std::string fmt = label + ": " + s.format;
                    changed = ImGui::SliderFloat("##v", &s.f, s.fmin, s.fmax, fmt.c_str());
                    break;
                }
            case Setting::Type::Int:
                {
                    ImGui::SetNextItemWidth(itemW);
                    const std::string fmt = label + ": %d";
                    changed = ImGui::SliderInt("##v", &s.i, s.imin, s.imax, fmt.c_str());
                    break;
                }
            case Setting::Type::Combo:
                {
                    ImGui::SetNextItemWidth(itemW);
                    std::vector<std::string> dynamic;
                    if (s.optionsFn) dynamic = s.optionsFn();
                    const std::vector<std::string>& opts = s.optionsFn ? dynamic : s.options;
                    const int cur = s.getIndex ? s.getIndex() : s.i;
                    const bool inRange = cur >= 0 && cur < static_cast<int>(opts.size());
                    const std::string preview = s.name + ": " + (inRange ? opts[static_cast<size_t>(cur)] : std::string());

                    // Popups use the global WindowPadding (0), so give them some air.
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.f, 6.f});
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 4.f});
                    if (ImGui::BeginCombo("##c", preview.c_str()))
                    {
                        for (int n = 0; n < static_cast<int>(opts.size()); ++n)
                        {
                            ImGui::PushID(n);
                            if (ImGui::Selectable(opts[static_cast<size_t>(n)].c_str(), n == cur))
                            {
                                if (s.setIndex) s.setIndex(n);
                                else s.i = n;
                                changed = true;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopStyleVar(2);
                    break;
                }
            case Setting::Type::Color:
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6.f, 6.f});
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 4.f});
                    changed = ImGui::ColorEdit4("##col", s.color.data(),
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                                ImGuiColorEditFlags_AlphaBar);
                    ImGui::PopStyleVar(2);
                    ImGui::SameLine(0.f, 8.f);
                    ImGui::TextUnformatted(s.name.c_str());
                    break;
                }
            case Setting::Type::Key:
                KeyButton(s.name, s.i, m, &s, itemW);
                break;

            case Setting::Type::Button:
                if (ImGui::Button(s.name.c_str(), {itemW, 0.f}) && s.onClick) s.onClick(m);
                break;

            case Setting::Type::Label:
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemW);
                ImGui::TextDisabled("%s", s.name.c_str());
                ImGui::PopTextWrapPos();
                break;

            case Setting::Type::Separator:
                if (s.name.empty()) ImGui::Separator();
                else ImGui::SeparatorText(s.name.c_str());
                break;

            case Setting::Type::Custom:
                if (s.draw) s.draw(m, itemW);
                break;
        }

        if (changed && s.onChange) s.onChange(m);
        if (!s.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", s.tooltip.c_str());
    }

    void DrawSettings(Module& m, float w)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();

        // Content goes on channel 1, the background rect (sized after layout) on channel 0.
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f, 6.f});
        ImGui::Indent(10.f);
        ImGui::Dummy({0.f, 2.f});

        const float itemW = w - 20.f;
        for (size_t idx = 0; idx < m.settings.size(); ++idx)
        {
            Setting& s = m.settings[idx];
            if (s.visibleIf && !s.visibleIf(m)) continue;
            ImGui::PushID(static_cast<int>(idx));
            DrawControl(m, s, itemW);
            ImGui::PopID();
        }

        ImGui::PushID("modkey");
        KeyButton(m.isAction ? "Hotkey" : "Toggle key", m.key, m, nullptr, itemW);
        ImGui::PopID();

        ImGui::Dummy({0.f, 2.f});
        ImGui::Unindent(10.f);
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        const float endY = ImGui::GetItemRectMax().y;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(start, {start.x + w, endY}, U32(theme::Menu().settingsBg));
        dl->ChannelsMerge();
    }

    void DrawModule(Module& m, float dt)
    {
        const theme::MenuColors& tc = theme::Menu();

        ImGui::PushID(&m);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = tc.rowHeight;

        ImGui::InvisibleButton("##row", {w, h});
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            m.Toggle();
            if (m.isAction) m.anim = 1.f; // flash on click
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m.expanded = !m.expanded;
        if (!m.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s\nLeft click: %s | Right click: settings",
                              m.description.c_str(), m.isAction ? "run" : "toggle");

        Approach(m.anim, (!m.isAction && m.enabled) ? 1.f : 0.f, dt);
        Approach(m.hover, hovered ? 1.f : 0.f, dt);

        ImVec4 bg = Mix(tc.rowBg, tc.rowHover, m.hover);
        bg = Mix(bg, Mix(tc.rowBg, tc.accent, 0.18f), m.anim);
        dl->AddRectFilled(p, {p.x + w, p.y + h}, U32(bg));
        dl->AddRectFilled(p, {p.x + 3.f * m.anim, p.y + h}, U32(tc.accent));

        const ImVec4 txt = Mix(tc.text, tc.textActive, std::max(m.anim, m.hover * 0.6f));
        const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
        dl->AddText({p.x + 10.f, ty}, U32(txt), m.name.c_str());
        dl->AddText({p.x + w - 16.f, ty}, U32(tc.textDim), m.expanded ? "-" : "+");

        if (m.expanded) DrawSettings(m, w);
        ImGui::PopID();
    }
}

void gui::ApplyStyle()
{
    theme::ApplyBaseStyle(); // fixed layout + default colors; JSON themes are layered on top by imjson
    theme::Apply(theme::Current()); // load the selected theme file (menu colors + imjson document)
}

bool gui::CaptureKey(int vk)
{
    if (!g_bind.mod) return false;

    int& target = g_bind.setting ? g_bind.setting->i : g_bind.mod->key;
    switch (vk)
    {
    case VK_INSERT: // closes the menu, which cancels the binding too
        return false;
    case VK_ESCAPE: // cancel: leave the existing binding untouched
        break;
    case VK_BACK:
    case VK_DELETE: // clear
        target = 0;
        break;
    default:
        target = vk;
        break;
    }
    g_bind = {};
    return true; // the key is consumed, the game never sees it
}

void gui::Render()
{
    modules::Render(); // HUD modules draw even when the menu is hidden

    if (!visible)
    {
        // closing the menu cancels a pending key binding
        g_bind = {};
        g_keyButtonHit = false;
        return;
    }

    theme::Begin(); // imjson pushes the theme onto ImGui's style stack
    const float dt = ImGui::GetIO().DeltaTime;
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    float x = 24.f;
    for (auto& tab : modules::Tabs())
    {
        if (tab->modules.empty()) continue;

        ImGui::SetNextWindowPos({x, 24.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({tab->width, 0.f}, {tab->width, FLT_MAX});
        if (ImGui::Begin(tab->name.c_str(), nullptr, kFlags))
        {
            for (auto& m : tab->modules) DrawModule(*m, dt);
        }
        ImGui::End();
        x += tab->width + 12.f;
    }
    theme::End(); // must be popped before ImGui::EndFrame()

    // Clicking anywhere except a bind button also cancels a pending binding.
    if (g_bind.mod && !g_keyButtonHit &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        g_bind = {};
    g_keyButtonHit = false;
}
