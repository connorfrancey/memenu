#include "config.h"
#include "core/module.h"

namespace {

class SaveConfig final : public ActionModule {
public:
    SaveConfig() : ActionModule("Save Config", "Misc", "Write settings to MedgeMenu.json", 40) {}
    void OnTrigger() override { config::Save(); }
};
MEM_MODULE(SaveConfig)

class LoadConfig final : public ActionModule {
public:
    LoadConfig() : ActionModule("Load Config", "Misc", "Reload settings from MedgeMenu.json", 50) {}
    void OnTrigger() override { config::Load(); }
};
MEM_MODULE(LoadConfig)

} // namespace
