#pragma once
#include "tas/replay.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The replay as a timeline: each key as the spans it is held (press to release), the axes as the frames they move on,
// and the camera keys. Edits rebuild the replay's events in one pass and mark the sync after them as unchecked.
// No ImGui and no game here (unit tested); the timeline window is features/tas/timeline_view.cpp.
namespace tas::timeline {

// Pressed on frame `start`, released on frame `end`. end == FrameCount(): still held when the replay ends.
struct Hold {
    std::uint32_t start = 0, end = 0;
    std::uint32_t Length() const { return end - start; }
    bool operator==(const Hold& o) const { return start == o.start && end == o.end; }
};

struct Track {
    std::uint16_t name = 0;
    bool axis = false;
    std::vector<Hold> holds;                // keys
    std::vector<std::uint32_t> moves;       // axes: the frames with movement on them
};

// Every name that has inputs, keys first, each group sorted by name. One pass over the events.
std::vector<Track> Tracks(const Replay& r);
std::vector<Hold> Holds(const Replay& r, std::uint16_t name);

// Replaces every press / release of `name` with `holds` (sorted, and merged where they touch or overlap; empty ones
// dropped). Frames are added at the end when a hold needs them. Other inputs are kept as they are.
void SetHolds(Replay& r, std::uint16_t name, std::vector<Hold> holds);

// ---- one frame -------------------------------------------------------------------------------------------------------------
bool HeldAt(const std::vector<Hold>& holds, std::uint32_t frame);
// Holds `name` on `frame`, or lets go of it there (a hold through it is split around it).
void SetHeld(Replay& r, std::uint16_t name, std::uint32_t frame, bool held);
// The movement of an axis on a frame (its events added up), and setting it: one event, or none for 0.
float AxisAt(const Replay& r, std::size_t frame, std::uint16_t name);
void SetAxis(Replay& r, std::size_t frame, std::uint16_t name, float value);

// ---- camera ----------------------------------------------------------------------------------------------------------------
// The look direction on `frame`, if a camera key covers it (from the first key to the last, inclusive).
bool CameraAt(const Replay& r, std::size_t frame, float& yaw, float& pitch);
bool IsLookAxis(const std::string& name);   // recorded input the camera keys replace (mouse, right stick)

// Adds a key, or replaces the one on its frame. Keys past the end are dropped.
void SetCameraKey(Replay& r, const CameraKey& key);
void EraseCameraKey(Replay& r, std::uint32_t frame);
// Moves the key on `from` to `to` (replacing one there). False if there's no key on `from`.
bool MoveCameraKey(Replay& r, std::uint32_t from, std::uint32_t to);
const CameraKey* FindCameraKey(const Replay& r, std::uint32_t frame);

// `deg` turned by whole turns to be as close as possible to `near`: keeps the camera from spinning the long way round.
float UnwrapNear(float deg, float near);

// ---- several at once (the timeline's selection) ---------------------------------------------------------------------------
struct KeyHold {
    std::uint16_t name = 0;
    Hold hold;
    bool operator==(const KeyHold& o) const { return name == o.name && hold == o.hold; }
};

// The hold of `name` that contains `h` now (after a merge with a neighbour), or `h` itself.
Hold Containing(const Replay& r, std::uint16_t name, const Hold& h);
// Moves the holds by `delta` frames, together (no earlier than frame 0: the delta is cut so the first one stops there).
// Returns them where they are now.
std::vector<KeyHold> MoveHolds(Replay& r, const std::vector<KeyHold>& holds, int delta);
void EraseHolds(Replay& r, const std::vector<KeyHold>& holds);
// Moves camera keys by `delta` frames, together (kept inside the replay). Returns their new frames.
std::vector<std::uint32_t> MoveCameraKeys(Replay& r, const std::vector<std::uint32_t>& frames, int delta);

// Copied holds and camera keys, relative to the first frame copied. Holds by key name, so they paste into any replay.
struct Clip {
    std::vector<std::pair<std::string, Hold>> holds;
    std::vector<CameraKey> camera;
    std::uint32_t length = 0;       // From the first frame to past the last
    bool Empty() const { return holds.empty() && camera.empty(); }
};
Clip Copy(const Replay& r, const std::vector<KeyHold>& holds, const std::vector<std::uint32_t>& cameraFrames);
// Pastes with the clip's first frame on `at` (adding frames at the end when needed). Returns what was pasted, for the
// selection: the holds as they are now, and the camera keys' frames.
std::vector<KeyHold> Paste(Replay& r, const Clip& clip, std::uint32_t at, std::vector<std::uint32_t>* cameraFrames = nullptr);

} // namespace tas::timeline
