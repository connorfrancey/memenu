#include "core/module.h"
#include "game/levels.h"

#include <imgui.h>

#include <string>

namespace {

// Name button: load + show / unload. Small button: visibility.
class Sublevels final : public ActionModule {
public:
    Sublevels() : ActionModule("Sublevels", "Levels", "Streaming sublevels of this level. Left click: refresh the list", 40)
    {
        expanded = true;
    }

    void OnTrigger() override { levels::Refresh(); }

private:
    static void DrawList(float width)
    {
        const auto subs = levels::Sublevels();
        if (subs.empty()) { ImGui::TextDisabled("No sublevels (open a level)"); return; }

        constexpr float kToggleW = 54.f, kGap = 4.f;
        for (std::size_t i = 0; i < subs.size(); ++i) {
            const auto& s = subs[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = s.name + (s.loaded ? " [L]" : "") + (s.visible ? " [V]" : "");
            if (ImGui::Button(label.c_str(), {width - kToggleW - kGap, 0.f})) levels::ToggleSublevelLoaded(s.name);
            ImGui::SameLine(0.f, kGap);
            if (ImGui::Button(s.visible ? "Hide" : "Show", {kToggleW, 0.f})) levels::ToggleSublevelVisible(s.name);
            ImGui::PopID();
        }
    }

    ctl::Custom list{this, [](float w) { DrawList(w); }};
    ctl::Label help{this, "Name button: load + show / unload. Show/Hide button: visibility. [L] loaded, [V] visible. "
                          "Streaming volumes can override these."};
};
MEM_MODULE(Sublevels)

} // namespace
