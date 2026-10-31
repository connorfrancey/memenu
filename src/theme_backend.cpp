#include "theme_backend.h"

#include <imjson.h>
#include <imgui.h>
#include <imgui_internal.h>

bool theme::backend::Push(const std::string& json)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const int colors = g.ColorStack.Size;
    const int vars   = g.StyleVarStack.Size;
    try {
        imjson::load_string(json.c_str());
        return true;
    } catch (...) {
        // Undo whatever got pushed before the failure, or ImGui asserts at EndFrame.
        if (g.ColorStack.Size > colors)    ImGui::PopStyleColor(g.ColorStack.Size - colors);
        if (g.StyleVarStack.Size > vars)   ImGui::PopStyleVar(g.StyleVarStack.Size - vars);
        return false;
    }
}

void theme::backend::Pop()
{
    imjson::pop_colors();
    imjson::pop_vars();
}
