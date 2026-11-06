#include "core/module.h"
#include "game/game.h"
#include "game/levels.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace {

// Shows whether the game integration is alive, which is the first thing to check when a game module does nothing.
class GameStatus final : public Module {
public:
    GameStatus() : Module("Game Status", "Misc", "Diagnostics for the game integration", 20)
    {
        enabledByDefault = true;
    }

    void OnRender() override
    {
        const game::Status st = game::GetStatus();
        char line[160];
        if (!st.ready)
            std::snprintf(line, sizeof line, "GAME: NOT READY (see MirrorsEdgeMenu.log)");
        else
            std::snprintf(line, sizeof line, "GAME: ready | ticks %llu | %s | pawn %s%s",
                          st.ticks, st.loading ? "loading" : "in level",
                          st.hasPawn ? "yes" : "NO",
                          st.hasPawn ? (" | hp " + std::to_string(st.health) + "/" + std::to_string(st.maxHealth)).c_str() : "");
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText({11.f, 31.f}, IM_COL32(0, 0, 0, 200), line);
        dl->AddText({10.f, 30.f}, st.ready ? IM_COL32(120, 230, 120, 255) : IM_COL32(255, 110, 100, 255), line);

        float y = 46.f;
        if (st.ready && showHooks) {    // The optional hooks: a feature built on a missing one does nothing
            std::snprintf(line, sizeof line, "hooks: state handler %s | death %s (%u) | script events %llu",
                          game::PlayerStateHooked() ? "on" : "OFF", game::DeathHooked() ? "on" : "OFF", st.deaths,
                          st.scriptEvents);
            const bool all = game::PlayerStateHooked() && game::DeathHooked();
            dl->AddText({11.f, y + 1.f}, IM_COL32(0, 0, 0, 200), line);
            dl->AddText({10.f, y}, all ? IM_COL32(170, 200, 170, 255) : IM_COL32(255, 190, 90, 255), line);
            y += 16.f;
        }
        const std::string msg = levels::LastMessage();     // result of the last level/sublevel action
        if (!msg.empty()) {
            dl->AddText({11.f, y + 1.f}, IM_COL32(0, 0, 0, 200), msg.c_str());
            dl->AddText({10.f, y}, IM_COL32(230, 230, 230, 255), msg.c_str());
        }
    }

private:
    ctl::Checkbox showHooks{this, "Show hooks", true, "Which optional engine hooks are on, deaths and script events seen"};
};
MEM_MODULE(GameStatus)

} // namespace
