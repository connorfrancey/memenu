#include "core/module.h"
#include "core/notify.h"
#include "game/game.h"

#include <imgui.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>

namespace {

// The game's console from the menu: runs a command as if typed there ("set Engine bEnableColorClear true",
// "stat fps", "slomo 0.5", "open edge_p"). The last commands are kept for running again with a click.
class Console final : public ActionModule {
public:
    Console() : ActionModule("Console", "Misc", "Run a console command in the game. Run: the command below", 15) {}

    void OnTrigger() override { Run(command.Get()); }

private:
    void Run(const std::string& cmd)
    {
        if (cmd.empty()) return;
        {
            std::lock_guard lock(mutex_);
            history_.erase(std::remove(history_.begin(), history_.end(), cmd), history_.end());
            history_.push_front(cmd);
            while (history_.size() > 8) history_.pop_back();
        }
        game::RunOnGameThread([cmd] {
            if (!game::ConsoleCommand(cmd)) notify::Push("Console: no player controller to run it");
        });
    }

    void DrawHistory(float width)
    {
        std::deque<std::string> h;
        {
            std::lock_guard lock(mutex_);
            h = history_;
        }
        if (h.empty()) return;
        ImGui::TextDisabled("Recent (click to run again)");
        for (std::size_t i = 0; i < h.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(h[i].c_str(), false, 0, {width, 0})) {
                command.Set(h[i]);
                Run(h[i]);
            }
            ImGui::PopID();
        }
    }

    std::mutex mutex_;
    std::deque<std::string> history_;   // Newest first

    ctl::Text command{this, "Command", "stat fps",
                      "Typed as in the game's console. Run the module (or its key) to send it"};
    ctl::Button run{this, "Run", [this] { Run(command.Get()); }};
    ctl::Custom recent{this, [this](float w) { DrawHistory(w); }};
};
MEM_MODULE(Console)

} // namespace
