#include "core/clock.h"
#include "core/module.h"
#include "core/notify.h"
#include "game/game.h"
#include "game/levels.h"
#include "game/save_library.h"

#include <imgui.h>

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

// Strang and Beamer armed when a state loads would fire right away in it (Load State drops them too)
void DropTools()
{
    for (const char* tool : {"Strang", "Beamer"})
        if (Module* m = modules::Find(tool)) m->SetEnabled(false);
}

void LoadEntry(int index)
{
    game::RunOnGameThread([index] {
        DropTools();
        std::string error;
        const auto list = save_library::List();
        if (save_library::Load(index, &error)) {
            if (index >= 0 && index < static_cast<int>(list.size())) notify::Push("Loaded " + list[index].name);
        } else {
            notify::Push("Load: " + error);
        }
    });
}

// A library of named save states per level, kept on disk. Saving or loading one makes it the Save State, so the Load
// State key keeps going back to it.
class SaveStates final : public ActionModule {
public:
    SaveStates()
        : ActionModule("Save States", "Trainer", "Named save states for each level, kept between sessions. Run: save a new one", 52)
    {
    }

    void OnTrigger() override
    {
        game::RunOnGameThread([] {
            std::string error;
            if (save_library::SaveNew(&error)) {
                const auto list = save_library::List();
                if (const int i = save_library::Selected(); i >= 0 && i < static_cast<int>(list.size()))
                    notify::Push("Saved " + list[i].name);
            } else {
                notify::Push("Save: " + error);
            }
        });
    }

private:
    void DrawList(float width)
    {
        const std::string map = levels::CurrentMap();
        const auto list = save_library::List(map);
        const int selected = save_library::Selected();
        if (map.empty() && list.empty()) {
            ImGui::TextDisabled("Open a level to see its states");
            return;
        }
        ImGui::TextDisabled("%zu state%s in %s", list.size(), list.size() == 1 ? "" : "s", save_library::Map().c_str());
        if (list.empty()) {
            ImGui::TextDisabled("None yet: run this or press Save new");
            return;
        }

        const float rowH = ImGui::GetFrameHeight();
        const float height = std::min(static_cast<float>(list.size() + 1), 10.f) * rowH + 6.f;
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##states", 2, kFlags, {width, height})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(list.size()); ++i) {
                const auto& e = list[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(e.name.c_str(), i == selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    save_library::Select(i);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) LoadEntry(i);
                }
                ImGui::SetItemTooltip("%s\nAt %.0f, %.0f, %.0f\nSaved %s\nDouble-click to load", e.name.c_str(), e.x, e.y,
                                      e.z, e.created.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%.0f", e.speed);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (selected < 0 || selected >= static_cast<int>(list.size())) return;
        const auto& sel = list[selected];

        // Rename: the box shows the selected state's name; Enter or leaving the box keeps what was typed
        if (renameFor_ != selected || (!renaming_ && renameName_ != sel.name)) {
            std::snprintf(rename_, sizeof rename_, "%s", sel.name.c_str());
            renameFor_ = selected;
            renameName_ = sel.name;
        }
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##name", "name", rename_, sizeof rename_);
        renaming_ = ImGui::IsItemActive();
        ImGui::SetItemTooltip("Name of the selected state");
        if (ImGui::IsItemDeactivated() && rename_[0]) {
            save_library::Rename(selected, rename_);
            renameName_ = rename_;
        }

        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float third = (width - 2 * gap) / 3.f;
        if (ImGui::Button("Load", {third, 0})) LoadEntry(selected);
        ImGui::SetItemTooltip("Load it (and make it the Save State: the Load State key comes back here)");
        ImGui::SameLine();
        if (ImGui::Button(confirm_ == Confirm::Overwrite && Pending() ? "Sure?" : "Overwrite", {third, 0})) {
            if (confirm_ == Confirm::Overwrite && Pending()) {
                confirm_ = Confirm::None;
                game::RunOnGameThread([selected] {
                    std::string error;
                    notify::Push(save_library::Overwrite(selected, &error) ? "State overwritten" : "Overwrite: " + error);
                });
            } else {
                Ask(Confirm::Overwrite);
            }
        }
        ImGui::SetItemTooltip("Replace it with where you are now");
        ImGui::SameLine();
        if (ImGui::Button(confirm_ == Confirm::Delete && Pending() ? "Sure?" : "Delete", {third, 0})) {
            if (confirm_ == Confirm::Delete && Pending()) {
                confirm_ = Confirm::None;
                save_library::Remove(selected);
            } else {
                Ask(Confirm::Delete);
            }
        }

        const float half = (width - gap) / 2.f;
        ImGui::BeginDisabled(selected == 0);
        if (ImGui::Button("Move up", {half, 0})) save_library::MoveBy(selected, -1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected + 1 >= static_cast<int>(list.size()));
        if (ImGui::Button("Move down", {half, 0})) save_library::MoveBy(selected, 1);
        ImGui::EndDisabled();
    }

    enum class Confirm { None, Overwrite, Delete };
    void Ask(Confirm what)
    {
        confirm_ = what;
        confirmAt_ = clk::Now();
    }
    bool Pending() const { return clk::SecondsSince(confirmAt_) < 3.0; }

    // Render thread
    char rename_[64] = "";
    int renameFor_ = -1;
    std::string renameName_;
    bool renaming_ = false;
    Confirm confirm_ = Confirm::None;
    clk::Ticks confirmAt_{};

    ctl::Custom table{this, [this](float w) { DrawList(w); }};
    ctl::Button saveNew{this, "Save new", [this] { OnTrigger(); }, "Save where you are as a new state"};
    ctl::Button loadSelected{this, "Load selected", [] { LoadEntry(save_library::Selected()); }};
    ctl::Button next{this, "Select next", [] { save_library::SelectNext(1); notify::Push(SelectedName()); }};
    ctl::Button previous{this, "Select previous", [] { save_library::SelectNext(-1); notify::Push(SelectedName()); }};
    ctl::Button openFolder{this, "Open save states folder", [] {
        std::error_code ec;
        const auto folder = save_library::Folder();
        std::filesystem::create_directories(folder, ec);
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }};

    static std::string SelectedName()
    {
        const auto list = save_library::List();
        const int i = save_library::Selected();
        return i >= 0 && i < static_cast<int>(list.size()) ? "State: " + list[i].name : "No save states here";
    }
};
MEM_MODULE(SaveStates)

} // namespace
