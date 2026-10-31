#include "core/module.h"

#include <imgui.h>

namespace {

class Watermark final : public Module {
public:
    Watermark() : Module("Watermark", "Misc", "Small text in the bottom-left corner", 10) {}

    void OnRender() override
    {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const int a = static_cast<int>(opacity.Get() * 255.f);
        ImGui::GetBackgroundDrawList()->AddText({10.f, ds.y - ImGui::GetFontSize() - 10.f}, IM_COL32(230, 50, 45, a),
                                                "Mirror's Edge Menu");
    }

private:
    ctl::Slider opacity{this, "Opacity", 0.6f, 0.1f, 1.f, "%.2f", 0.05f};
};
MEM_MODULE(Watermark)

} // namespace
