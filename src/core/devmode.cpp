#include "devmode.h"

#include "module.h"
#include "unload.h"

#include <spdlog/spdlog.h>

#include <atomic>

namespace {

std::atomic<bool> g_enabled{false};

class DeveloperMode final : public Module {
public:
    DeveloperMode()
        : Module(devmode::kModuleName, "Misc",
                 "Developer tools in the menu: the Test module (in-game self-tests with an output console). Saved with "
                 "the config; a developer build (memenu --dev) turns it on at every start",
                 900)
    {
        enabledByDefault = MEMENU_DEV_MODE != 0;
    }

    void OnEnable() override
    {
        g_enabled = true;
        spdlog::info("Developer mode on");
    }

    void OnDisable() override
    {
        g_enabled = false;
        if (!unload::Requested()) spdlog::info("Developer mode off");   // Not every module going off at unload (it stays stored)
    }
};
MEM_MODULE(DeveloperMode)

} // namespace

bool devmode::Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void devmode::ApplyBuild()
{
#if MEMENU_DEV_MODE
    Module* m = modules::Find(kModuleName);
    if (m && !m->Enabled()) {
        m->SetEnabled(true);
        spdlog::info("Developer build: developer mode switched on (stored in the config)");
    }
#endif
}
