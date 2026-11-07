#pragma once
#include <functional>

struct IDirect3DDevice9;

namespace hooks {
    bool Install();
    void Uninstall();
    unsigned EndSceneCalls();   // How often the EndScene hook ran: 0 a few seconds after Install means it never fires
    // Runs `fn` on the render thread inside the next ImGui frame (before the menu draws), where clicks in the menu run.
    // Dropped if the hooks are removed first.
    void RunOnRenderThread(std::function<void()> fn);
    // Runs `fn` once with the game's device, on the render thread at the next EndScene (TAS snapshots hook it).
    void OnDevice(void (*fn)(IDirect3DDevice9*));
    // Runs before every device Reset (render thread): whatever holds D3DPOOL_DEFAULT objects must let go of them.
    void OnBeforeReset(void (*fn)());

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
