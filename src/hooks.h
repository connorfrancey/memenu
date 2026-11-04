#pragma once
#include <functional>

namespace hooks {
    bool Install();
    void Uninstall();
    unsigned EndSceneCalls();   // How often the EndScene hook ran: 0 a few seconds after Install means it never fires
    // Runs `fn` on the render thread inside the next ImGui frame (before the menu draws), where clicks in the menu run.
    // Dropped if the hooks are removed first.
    void RunOnRenderThread(std::function<void()> fn);

    // While the game thread stands still (a paused TAS session blocks it at the start of a frame) the game draws
    // nothing, and the menu with it. CaptureFrames(true) keeps a copy of every frame; DrawWhileStalled(), called on the
    // stalled thread every few ms, then draws the menu over the last frame and presents it. It only draws once the
    // renderer has been idle for a few calls; true when it presented a frame.
    void CaptureFrames(bool on);
    bool DrawWhileStalled();
    unsigned StalledFrames();
// Vertices the menu's ImGui frame drew last time (the menu window, every module's overlay): the self-test's "does this
// module draw" check.
unsigned MenuVertices();   // frames DrawWhileStalled presented so far (the self-test checks pausing with it)
}
