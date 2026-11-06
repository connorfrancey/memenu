// Hotkey List: every key the menu uses, in one window. Module toggle keys, the keys given to controls and the key
// settings of modules (TAS, flight, speedrun timer...), searchable, rebindable in place, with the ones that clash
// marked. See core/bindings.h.
#include "core/bindings.h"
#include "core/keybinds.h"
#include "core/keys.h"
#include "core/module.h"
#include "gui.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

class HotkeyList final : public Module {
public:
    HotkeyList() : Module("Hotkey List", "Misc", "Every key the menu uses in one window: search, rebind, find clashes", 25) {}

    void OnRender() override
    {
        if (!gui::visible) return;
        bool open = true;
        ImGui::SetNextWindowSize({460.f, 520.f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Hotkeys##list", &open)) DrawWindow();
        ImGui::End();
        if (!open) SetEnabled(false);
    }

private:
    void DrawWindow()
    {
        const std::vector<bindings::Slot>& all = bindings::All();
        int boundCount = 0;
        for (const auto& s : all) boundCount += *s.binding != 0;
        const int clashes = bindings::ClashCount();

        ImGui::TextDisabled("%d keys bound", boundCount);
        ImGui::SameLine();
        if (clashes > 0) ImGui::TextColored({1.f, 0.72f, 0.3f, 1.f}, "%d clash%s", clashes, clashes == 1 ? "" : "es");
        else ImGui::TextColored({0.4f, 1.f, 0.5f, 1.f}, "no clashes");
        ImGui::SetItemTooltip("Keys bound to more than one thing do all of them. A TAS session's keys are the "
                              "exception: while it runs they only control the session");

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search: module, action or key (\"ctrl\", \"F5\")", search_, sizeof search_);
        ImGui::Checkbox("Bound only", &boundOnly_);
        ImGui::SameLine();
        ImGui::Checkbox("Clashes only", &clashesOnly_);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        ImGui::SetItemTooltip("Click a key to bind it: hold Ctrl / Shift / Alt for a combo, mouse buttons 3-5 work too.\n"
                              "Right click clears. Esc cancels.");

        const std::string query = Lower(search_);
        if (!ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                               {0.f, ImGui::GetContentRegionAvail().y}))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableHeadersRow();

        int shown = 0;
        for (const bindings::Slot& s : all) {
            const int bound = *s.binding;
            if (boundOnly_ && bound == 0) continue;
            if (clashesOnly_ && bindings::ClashesWith(bound, s.binding).empty()) continue;
            if (!query.empty()) {
                const std::string text = Lower(s.module->Tab() + " " + s.module->Name() + " " + s.what + " " +
                                               (bound ? keys::Name(bound) : std::string()));
                if (text.find(query) == std::string::npos) continue;
            }
            ++shown;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(s.module->Name().c_str());
            ImGui::SetItemTooltip("%s tab", s.module->Tab().c_str());
            ImGui::TableNextColumn();
            keybinds::Button(s.what, *s.binding, -FLT_MIN);
        }
        if (shown == 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Nothing matches");
        }
        ImGui::EndTable();
    }

    char search_[64] = "";
    bool boundOnly_ = true;
    bool clashesOnly_ = false;
};
MEM_MODULE(HotkeyList)

} // namespace
