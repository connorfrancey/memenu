#include "core/module.h"
#include "game/levels.h"

namespace {

class RestartCheckpoint final : public ActionModule {
public:
    RestartCheckpoint() : ActionModule("Restart Checkpoint", "Levels", "Reload from the last checkpoint", 20) {}
    void OnTrigger() override { levels::RestartFromLastCheckpoint(); }
};
MEM_MODULE(RestartCheckpoint)

class MainMenu final : public ActionModule {
public:
    MainMenu() : ActionModule("Main Menu", "Levels", "Quit to the main menu", 30) {}
    void OnTrigger() override { levels::QuitToMainMenu(); }
};
MEM_MODULE(MainMenu)

} // namespace
