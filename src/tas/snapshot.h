#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Memory snapshots of the game, for the TAS: the whole state of the game at the start of a frame, put back exactly.
//
// What is held: the game's own memory (see memory_map.h): UE3's allocations (tracked with write watching by the
// loader from the start of the process), its CRT heaps and its modules' data. Everything else keeps running: the
// menu, Windows, the graphics driver, the sound. Around them:
//   - The rendering thread is switched off for a session (the engine renders on the game thread), and the game's
//     worker threads (engine pool, PhysX) are suspended while a snapshot is taken or put back, at a moment all of them
//     wait for work.
//   - Memory the game frees and handles it closes are held back during a session, and Direct3D objects it releases
//     kept alive (tas/d3d_pins.h): an older snapshot may still refer to them.
//   - After a restore, the game's next new allocations get the addresses they got the first time those frames ran,
//     so running the same frames again gives the same memory, byte for byte.
//
// Storage: a full copy when the session begins, then keyframes that hold only the pages written since the previous
// one (with the same pages changing every frame, a few MB), zstd compressed, in page file backed sections outside the
// game's address space.
//
// Game thread only, at the start of a frame (the TAS's frame handler), except Available/Status.
namespace tas::snapshot {

bool Init();                     // once, after the TAS engine is up: finds the loader's tracker
bool Available();                // the loader tracks the game's memory with write watching
std::string WhyUnavailable();

// ---- session --------------------------------------------------------------------------------------------------------
// Starts keeping snapshots: the state now becomes keyframe `frame` (the base: every page of the game). False if it
// couldn't (then nothing changed).
bool Begin(std::uint32_t frame);
// Drops every snapshot, lets go of what was held back, rendering thread back on.
void End();
bool Active();
// At unload, after the session ended: the compression workers go too.
void Shutdown();
// The engine's rendering thread was switched off for snapshots (it stays off until the game closes). The engine renders
// on the game thread then; mmultiplayer's death hook functions aren't reached that way (deaths happen, unseen by it).
bool RenderingThreadOff();
// The frame that runs now (stamps what the game frees, closes and allocates during it). Call at the start of each frame.
void SetFrame(std::uint32_t frame);

// ---- keyframes ------------------------------------------------------------------------------------------------------
// Stores the state now as keyframe `frame`. `frame` must be after every keyframe kept (DropAfter first).
bool Capture(std::uint32_t frame);
// Puts back keyframe `frame` (must exist). The state is then exactly what it was at the start of that frame.
bool Restore(std::uint32_t frame);
// The newest keyframe at or before `frame`.
std::optional<std::uint32_t> Nearest(std::uint32_t frame);
bool Has(std::uint32_t frame);
// The inputs of `frame` changed: keyframes after it are no longer what that timeline reaches.
void DropAfter(std::uint32_t frame);
// The frame the game's state is anchored to: the last keyframe taken or put back (the state is that keyframe plus
// whatever ran since).
std::optional<std::uint32_t> Anchor();

// Compares the state now with keyframe `frame`, which this run reached again (same inputs from an older keyframe).
// Returns the number of pages that differ (0: identical). With `snap`, differing pages are put back to the keyframe and
// it becomes the anchor, so the run goes on exactly as recorded.
std::size_t Verify(std::uint32_t frame, bool snap);

// ---- status ---------------------------------------------------------------------------------------------------------
struct Stats {
    bool active = false;
    std::size_t keyframes = 0;
    std::uint64_t storedBytes = 0;       // compressed, every keyframe
    std::uint64_t baseBytes = 0;         // uncompressed size of the base
    std::size_t lastPages = 0;           // pages in the last capture / restore
    double lastCaptureMs = 0.0, lastRestoreMs = 0.0, beginMs = 0.0;
    std::size_t restores = 0;
    std::size_t verifiedPages = 0, mismatchedPages = 0;   // over every Verify
    std::size_t replayedAllocs = 0;
    std::size_t heldDecommits = 0;
    std::string lastError;
};
Stats GetStats();                         // any thread

// Called on the game thread after every restore: whatever caches pointers into the game must drop them (the objects
// may be somewhere else, or gone, in the state put back).
void OnRestore(void (*callback)());

// How much the keyframes may take (compressed, outside the game's address space) before older ones are thinned out:
// the ones far from where the session is go first, so going back nearby stays instant and far back takes a few frames
// more to run. The first keyframe (the level start) always stays.
void SetBudget(std::uint64_t bytes);

} // namespace tas::snapshot
