#include "keybinds.h"
#include "bindings.h"
#include "keys.h"

#include <Windows.h>
#include <imgui.h>

namespace {
    int* g_target = nullptr;        // The key variable waiting for a press
    bool g_buttonHit = false;       // A click this frame landed on a bind button
    int g_modifier = 0;             // A modifier pressed while waiting: bound on its own if released without a key

    void Finish()
    {
        g_target = nullptr;
        g_modifier = 0;
    }

    // "Ctrl+Shift+" for the modifiers held now
    std::string HeldPrefix()
    {
        const std::string name = keys::Name(keys::HeldMods() | VK_SPACE);
        return name.substr(0, name.rfind('+') + 1);
    }

    bool g_mouseWas[3] = {};
    constexpr int kMouseButtons[3] = {VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
}

void keybinds::Button(const std::string& caption, int& key, float width)
{
    const bool armed = g_target == &key;
    std::string text;
    if (armed) {
        const int mods = keys::HeldMods();
        text = mods ? HeldPrefix() + "... (a key)" : std::string("Press a key or combo... Esc=cancel");
    } else {
        text = caption + ": " + keys::Name(key);
    }
    const std::vector<std::string> clashes = armed ? std::vector<std::string>{} : bindings::ClashesWith(key, &key);

    ImGui::PushID(&key);
    if (!clashes.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.f, 0.72f, 0.3f, 1.f});
    if (ImGui::Button((text + "##key").c_str(), {width, 0.f})) {
        if (armed) Finish();
        else {
            g_target = &key;
            g_modifier = 0;
        }
    }
    if (!clashes.empty()) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) g_buttonHit = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        key = 0;
        if (armed) Finish();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !armed) {
        std::string tip;
        if (!clashes.empty()) {
            tip = keys::Name(key) + " also does:";
            for (const auto& c : clashes) tip += "\n  " + c;
            tip += "\n";
        }
        tip += "Click: bind (Ctrl / Shift / Alt combos and mouse buttons 3-5 work)\nRight click: clear";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::PopID();
}

bool keybinds::Capture(int vk)
{
    if (!g_target) return false;

    if (vk == VK_INSERT) return false;      // Closes the menu, which cancels the binding too
    if (keys::IsModifier(vk)) {             // Wait for the rest of the combo (EndFrame binds it alone if released)
        g_modifier = vk == VK_LSHIFT || vk == VK_RSHIFT ? VK_SHIFT
                   : vk == VK_LCONTROL || vk == VK_RCONTROL ? VK_CONTROL
                   : vk == VK_LMENU || vk == VK_RMENU ? VK_MENU : vk;
        return true;
    }
    const int mods = keys::HeldMods();
    if (mods == 0 && vk == VK_ESCAPE) {
        // Cancel: keep the existing binding
    } else if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) {
        *g_target = 0;                      // Clear
    } else {
        *g_target = vk | mods;
    }
    Finish();
    return true;            // Consumed: the game never sees it
}

bool keybinds::Armed() { return g_target != nullptr; }

void keybinds::Cancel()
{
    Finish();
    g_buttonHit = false;
}

void keybinds::EndFrame()
{
    // A modifier pressed and let go with no key in between: it is the binding
    if (g_target && g_modifier && !(GetAsyncKeyState(g_modifier) & 0x8000)) {
        *g_target = g_modifier;
        Finish();
    }
    if (g_target && !g_buttonHit &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        Finish();
    g_buttonHit = false;
}

std::vector<int> keybinds::PollMouseButtons(bool focused)
{
    std::vector<int> pressed;
    for (int i = 0; i < 3; ++i) {
        const bool down = (GetAsyncKeyState(kMouseButtons[i]) & 0x8000) != 0;
        if (down && !g_mouseWas[i] && focused) pressed.push_back(kMouseButtons[i]);
        g_mouseWas[i] = down;
    }
    return pressed;
}
