#include "core/module.h"

#include <imgui.h>

#include <string>

namespace {

// The trainer's "Show Tool Overlay": which tools are armed or on, in the top-right corner.
class ToolOverlay final : public Module {
public:
    ToolOverlay() : Module("Tool Overlay", "Trainer", "Show the armed tools in the top-right corner", 80)
    {
        enabledByDefault = true;
    }

    void OnRender() override
    {
        static constexpr struct { const char* module; const char* label; } kShown[] = {
            {"Strang", "Strang"}, {"Beamer", "Beamer"}, {"Kickglitch", "KG"},
            {"Flight", "Flight"}, {"No Clip", "No Clip"}, {"God Mode", "God"},
        };

        std::string text;
        for (const auto& s : kShown) {
            const Module* m = modules::Find(s.module);
            if (!m || !m->Enabled()) continue;
            if (!text.empty()) text += ' ';
            text += s.label;
        }
        if (text.empty()) return;

        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddRectFilled({ds.x - ts.x - 10.f, 0.f}, {ds.x, ts.y + 4.f}, IM_COL32(0, 0, 0, 100));
        dl->AddText({ds.x - ts.x - 5.f, 2.f}, color.Packed(), text.c_str());
    }

private:
    ctl::ColorPick color{this, "Text color", {1.f, 1.f, 1.f, 1.f}};
};
MEM_MODULE(ToolOverlay)

} // namespace
