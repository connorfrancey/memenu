#include "core/module.h"
#include "game/levels.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The level's streaming sublevels as a table: whether each is loaded, and whether it is shown. Ticking Loaded loads and
// shows it (unticking unloads it); Visible only shows or hides it.
class Sublevels final : public ActionModule {
public:
    Sublevels() : ActionModule("Sublevels", "Levels", "Streaming sublevels of this level. Run: read the list again", 40)
    {
        expanded = true;
    }

    void OnTrigger() override { levels::Refresh(); }

private:
    void DrawList(float width)
    {
        const auto subs = levels::Sublevels();
        if (subs.empty()) {
            ImGui::TextDisabled("No sublevels (open a level)");
            return;
        }

        const auto loaded = std::count_if(subs.begin(), subs.end(), [](const auto& s) { return s.loaded; });
        const auto visible = std::count_if(subs.begin(), subs.end(), [](const auto& s) { return s.visible; });
        ImGui::TextDisabled("%zu sublevels, %td loaded, %td visible", subs.size(), loaded, visible);
        ImGui::SetItemTooltip("Streaming volumes in the level load and show them as you move: they can change these back");

        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##find", "find", find_, sizeof find_);
        const std::string find = Lower(find_);

        // At most about 12 rows tall: the column stays usable in levels with dozens of parts
        const float rowH = ImGui::GetFrameHeight();
        const float height = std::min(static_cast<float>(subs.size() + 1), 12.f) * rowH + 6.f;
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##subs", 3, kFlags, {width, height})) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Sublevel", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < subs.size(); ++i) {
            const auto& s = subs[i];
            if (!find.empty() && Lower(s.name).find(find) == std::string::npos) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (s.visible) ImGui::TextUnformatted(s.name.c_str());
            else ImGui::TextDisabled("%s", s.name.c_str());
            ImGui::SetItemTooltip("%s", s.name.c_str());

            ImGui::TableNextColumn();
            bool isLoaded = s.loaded;
            if (ImGui::Checkbox("##loaded", &isLoaded)) levels::ToggleSublevelLoaded(s.name);
            ImGui::SetItemTooltip(s.loaded ? "Unload it" : "Load and show it");

            ImGui::TableNextColumn();
            bool isVisible = s.visible;
            if (ImGui::Checkbox("##visible", &isVisible)) levels::ToggleSublevelVisible(s.name);
            ImGui::SetItemTooltip(s.visible ? "Hide it (stays loaded)" : "Show it (loads it if needed)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    char find_[48] = "";
    ctl::Custom list{this, [this](float w) { DrawList(w); }};
};
MEM_MODULE(Sublevels)

} // namespace
