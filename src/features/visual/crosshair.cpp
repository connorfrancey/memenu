#include "core/module.h"

#include <imgui.h>

namespace {

class Crosshair final : public Module {
public:
    Crosshair() : Module("Crosshair", "Visual", "Draw a crosshair in the screen center", 30)
    {
        gap.VisibleIf([this] { return style.Get() == 0; });
        thickness.VisibleIf([this] { return style.Get() != 1; });
    }

    void OnRender() override
    {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const ImVec2 c{ds.x * 0.5f, ds.y * 0.5f};
        const float s = static_cast<float>(size.Get());
        const float g = gap, th = thickness;
        const ImU32 col = color.Packed();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        switch (style.Get()) {
            case 0:
                dl->AddLine({c.x - g - s, c.y}, {c.x - g, c.y}, col, th);
                dl->AddLine({c.x + g, c.y}, {c.x + g + s, c.y}, col, th);
                dl->AddLine({c.x, c.y - g - s}, {c.x, c.y - g}, col, th);
                dl->AddLine({c.x, c.y + g}, {c.x, c.y + g + s}, col, th);
                break;
            case 1: dl->AddCircleFilled(c, s * 0.25f, col); break;
            default: dl->AddCircle(c, s, col, 0, th); break;
        }
    }

private:
    ctl::Combo style{this, "Style", {"Cross", "Dot", "Circle"}};
    ctl::SliderInt size{this, "Size", 8, 2, 40};
    ctl::Slider gap{this, "Gap", 3.f, 0.f, 20.f, "%.0f", 1.f};
    ctl::Slider thickness{this, "Thickness", 2.f, 1.f, 6.f, "%.1f", 0.5f};
    ctl::ColorPick color{this, "Color", {0.9f, 0.2f, 0.18f, 1.f}};
};
MEM_MODULE(Crosshair)

} // namespace
