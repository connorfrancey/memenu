#pragma once
#include <functional>

namespace hooks {
    bool Install();
    void Uninstall();
    unsigned EndSceneCalls();   // How often the EndScene hook ran: 0 a few seconds after Install means it never fires
    // Runs `fn` on the render thread inside the next ImGui frame (before the menu draws), where clicks in the menu run.
    // Dropped if the hooks are removed first.
    void RunOnRenderThread(std::function<void()> fn);
}
