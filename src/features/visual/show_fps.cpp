#include "core/module.h"

#include <imgui.h>

#include <cstdio>

namespace {

class ShowFps final : public Module {
public:
    ShowFps() : Module("Show FPS", "Visual", "FPS counter", 20)
    {
        enabledByDefault = true;
    }

    void OnRender() override
    {
        const ImGuiIO& io = ImGui::GetIO();
        char buf[64];
        if (frametime)
            std::snprintf(buf, sizeof buf, "%.0f FPS (%.2f ms)", io.Framerate, 1000.f / io.Framerate);
        else
            std::snprintf(buf, sizeof buf, "%.0f FPS", io.Framerate);

        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const int c = corner;                                  // bit0 = right, bit1 = bottom
        const ImVec2 pos{(c & 1) ? io.DisplaySize.x - ts.x - 10.f : 10.f,
                         (c & 2) ? io.DisplaySize.y - ts.y - 10.f : 10.f};
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText({pos.x + 1.f, pos.y + 1.f}, IM_COL32(0, 0, 0, 200), buf);
        dl->AddText(pos, color.Packed(), buf);
    }

private:
    ctl::Combo corner{this, "Corner", {"Top left", "Top right", "Bottom left", "Bottom right"}};
    ctl::ColorPick color{this, "Text color", {1.f, 1.f, 1.f, 1.f}};
    ctl::Checkbox frametime{this, "Frametime"};
};
MEM_MODULE(ShowFps)

} // namespace
