#pragma once
#include <cstdint>

struct IDirect3DDevice9;

// Direct3D objects across TAS memory snapshots (tas/snapshot.h).
//
// The game's memory refers to its textures, buffers, shaders and queries by pointer, but the objects themselves live in
// the Direct3D runtime and the driver, which a snapshot doesn't hold. Putting back an older snapshot would leave the
// game holding objects it had released since (destroyed: a crash) and forget objects it made since (leaked).
//
// So during a session no object of the game's is destroyed: every object it creates or releases gets one extra
// reference of ours (a pin). Each keyframe records the game's reference count of the objects seen; a restore sets the
// counts back to the keyframe's (objects made after it go back to 0, kept alive only by the pin). When the session ends
// the pins are dropped and objects nobody holds any more are destroyed then.
//
// The menu's own objects (ImGui's) are left alone: their callers are the menu.
namespace tas::d3dpins {

bool Install(IDirect3DDevice9* device);   // hooks Create* on the device and AddRef/Release of each object type
bool Installed();

// Game thread. Frames are the TAS's.
void Begin();
void SetFrame(std::uint32_t frame);       // the frame running now (objects created in it are stamped with it)
void Capture(std::uint32_t keyframe);
void Restore(std::uint32_t keyframe);
void DropAfter(std::uint32_t frame);
void Forget(std::uint32_t keyframe);      // a keyframe thinned out
void End();                               // drops every pin
bool Active();

struct Stats {
    std::uint32_t tracked = 0, created = 0, adjusted = 0, hookedTypes = 0;
};
Stats GetStats();

} // namespace tas::d3dpins
