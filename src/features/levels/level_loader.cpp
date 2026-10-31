#include "core/module.h"
#include "game/levels.h"

#include <imgui.h>

#include <string>

namespace {

// Click a level's header to open it, then click "Start of level" or a checkpoint to load it. Maps and checkpoints come
// from the game's own level-select data; loads use the same StartGame() the game's menu does.
class LevelLoader final : public ActionModule {
public:
    LevelLoader() : ActionModule("Levels", "Levels", "Left click: refresh the lists. Click a level below to load it", 10)
    {
        expanded = true;
    }

    void OnTrigger() override { levels::Refresh(); }

private:
    void DrawList(float width) const
    {
        const auto method = consoleLoad ? levels::Method::Console : levels::Method::Game;
        const bool save = saveProgress;

        const auto maps = levels::Maps();
        if (maps.empty()) { ImGui::TextDisabled("No maps yet"); return; }

        for (int i = 0; i < static_cast<int>(maps.size()); ++i) {
            const auto& map = maps[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            if (ImGui::CollapsingHeader(map.label.c_str())) {
                if (ImGui::Button("Start of level", {width, 0.f})) levels::Load(i, 0, method, save);
                for (int j = 0; j < static_cast<int>(map.checkpoints.size()); ++j) {
                    ImGui::PushID(j);
                    if (ImGui::Button(map.checkpoints[static_cast<std::size_t>(j)].c_str(), {width, 0.f}))
                        levels::Load(i, j + 1, method, save);
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
    }

    ctl::Custom list{this, [this](float w) { DrawList(w); }};
    ctl::Checkbox consoleLoad{this, "Console load", false,
                              "Use the console command 'open <map>' instead of the game's StartGame. Ignores checkpoints"};
    ctl::Checkbox saveProgress{this, "Save progress", false, "Let loads made here overwrite your saved checkpoint progress"};
};
MEM_MODULE(LevelLoader)

} // namespace
