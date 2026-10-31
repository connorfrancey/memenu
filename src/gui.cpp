#include "gui.h"
#include "core/keybinds.h"
#include "core/module.h"
#include "core/notify.h"
#include "core/setting.h"
#include "theme.h"

#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <string>
#include <imgui.h>

namespace
{
    ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }
    ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
    void Approach(float& v, float target, float dt) { v += (target - v) * std::min(1.f, dt * 14.f); }

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
        for (ctl::Setting* s : m.Settings())
            if (s->Visible()) s->Show(itemW);

        keybinds::Button(m.IsAction() ? "Hotkey" : "Toggle key", m.key, itemW);

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
            if (m.IsAction()) m.anim = 1.f; // flash on click
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m.expanded = !m.expanded;
        if (!m.Description().empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s\nLeft click: %s | Right click: settings",
                              m.Description().c_str(), m.IsAction() ? "run" : "toggle");

        Approach(m.anim, (!m.IsAction() && m.Enabled()) ? 1.f : 0.f, dt);
        Approach(m.hover, hovered ? 1.f : 0.f, dt);

        ImVec4 bg = Mix(tc.rowBg, tc.rowHover, m.hover);
        bg = Mix(bg, Mix(tc.rowBg, tc.accent, 0.18f), m.anim);
        dl->AddRectFilled(p, {p.x + w, p.y + h}, U32(bg));
        dl->AddRectFilled(p, {p.x + 3.f * m.anim, p.y + h}, U32(tc.accent));

        const ImVec4 txt = Mix(tc.text, tc.textActive, std::max(m.anim, m.hover * 0.6f));
        const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
        dl->AddText({p.x + 10.f, ty}, U32(txt), m.Name().c_str());
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
    return keybinds::Capture(vk);
}

void gui::Render()
{
    modules::Render(); // HUD modules draw even when the menu is hidden
    notify::Draw();

    if (!visible)
    {
        keybinds::Cancel(); // closing the menu cancels a pending key binding
        return;
    }

    theme::Begin(); // imjson pushes the theme onto ImGui's style stack
    const float dt = ImGui::GetIO().DeltaTime;
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    float x = 24.f;
    bool anyTab = false;
    for (auto& tab : modules::Tabs())
    {
        if (tab->modules.empty()) continue;
        anyTab = true;

        ImGui::SetNextWindowPos({x, 24.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({tab->width, 0.f}, {tab->width, FLT_MAX});
        if (ImGui::Begin(tab->name.c_str(), nullptr, kFlags))
        {
            for (Module* m : tab->modules) DrawModule(*m, dt);
        }
        ImGui::End();
        x += tab->width + 12.f;
    }

    // Built with every feature off (MEMENU_FEATURES=OFF): still show that the menu is loaded
    if (!anyTab)
    {
        ImGui::SetNextWindowPos({x, 24.f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Mirror's Edge Menu", nullptr, kFlags))
            ImGui::TextDisabled("Built without features");
        ImGui::End();
    }
    theme::End(); // must be popped before ImGui::EndFrame()

    keybinds::EndFrame(); // clicking anywhere except a bind button cancels a pending binding
}
