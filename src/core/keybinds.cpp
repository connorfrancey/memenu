#include "keybinds.h"
#include "keys.h"

#include <Windows.h>
#include <imgui.h>

namespace {
    int* g_target = nullptr;        // The key variable waiting for a press
    bool g_buttonHit = false;       // A click this frame landed on a bind button
}

void keybinds::Button(const std::string& caption, int& key, float width)
{
    const bool armed = g_target == &key;
    const std::string text = armed ? std::string("Press key... Esc=cancel") : caption + ": " + keys::Name(key);

    ImGui::PushID(&key);
    if (ImGui::Button((text + "##key").c_str(), {width, 0.f})) g_target = armed ? nullptr : &key;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) g_buttonHit = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        key = 0;
        if (armed) g_target = nullptr;
    }
    ImGui::PopID();
}

bool keybinds::Capture(int vk)
{
    if (!g_target) return false;

    switch (vk) {
        case VK_INSERT:     // Closes the menu, which cancels the binding too
            return false;
        case VK_ESCAPE:     // Cancel: keep the existing binding
            break;
        case VK_BACK:
        case VK_DELETE:     // Clear
            *g_target = 0;
            break;
        default:
            *g_target = vk;
            break;
    }
    g_target = nullptr;
    return true;            // Consumed: the game never sees it
}

bool keybinds::Armed() { return g_target != nullptr; }

void keybinds::Cancel()
{
    g_target = nullptr;
    g_buttonHit = false;
}

void keybinds::EndFrame()
{
    if (g_target && !g_buttonHit &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        g_target = nullptr;
    g_buttonHit = false;
}
