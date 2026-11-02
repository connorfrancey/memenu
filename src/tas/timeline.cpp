#include "tas/timeline.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace tas::timeline {

namespace {

bool IsKeyEdge(EventKind k) { return k == EventKind::Pressed || k == EventKind::Released; }

// Builds the holds of a key from its press/release events, in frame order.
struct HoldBuilder {
    std::vector<Hold> holds;
    bool open = false;
    std::uint32_t since = 0;

    void Add(EventKind kind, std::uint32_t frame)
    {
        if (kind == EventKind::Pressed && !open) {
            open = true;
            since = frame;
        } else if (kind == EventKind::Released && open) {
            open = false;
            if (frame > since) holds.push_back({since, frame});
            else holds.push_back({since, since + 1});   // pressed and released on one frame: shown as one frame
        }
    }
    void Finish(std::uint32_t frameCount)
    {
        if (open) holds.push_back({since, std::max(frameCount, since + 1)});
        open = false;
    }
};

std::vector<Hold> Normalize(std::vector<Hold> holds)
{
    holds.erase(std::remove_if(holds.begin(), holds.end(), [](const Hold& h) { return h.end <= h.start; }), holds.end());
    std::sort(holds.begin(), holds.end(), [](const Hold& a, const Hold& b) { return a.start < b.start; });
    std::vector<Hold> out;
    for (const Hold& h : holds) {
        if (!out.empty() && h.start <= out.back().end) out.back().end = std::max(out.back().end, h.end);
        else out.push_back(h);
    }
    return out;
}

float Ease01(Ease ease, float t)
{
    switch (ease) {
        case Ease::Linear: return t;
        case Ease::Smooth: return t * t * (3.f - 2.f * t);
        case Ease::Hold:   return 0.f;
    }
    return t;
}

} // namespace

std::vector<Track> Tracks(const Replay& r)
{
    std::map<std::uint16_t, HoldBuilder> keys;
    std::map<std::uint16_t, std::vector<std::uint32_t>> axes;
    for (std::size_t f = 0; f < r.frames.size(); ++f) {
        const Frame& fr = r.frames[f];
        for (std::uint32_t i = 0; i < fr.count; ++i) {
            const Event& e = r.events[fr.first + i];
            if (e.kind == EventKind::Axis) {
                auto& moves = axes[e.name];
                if (moves.empty() || moves.back() != f) moves.push_back(static_cast<std::uint32_t>(f));
            } else if (IsKeyEdge(e.kind)) {
                keys[e.name].Add(e.kind, static_cast<std::uint32_t>(f));
            }
        }
    }

    std::vector<Track> out;
    for (auto& [name, b] : keys) {
        b.Finish(static_cast<std::uint32_t>(r.frames.size()));
        Track t;
        t.name = name;
        t.holds = std::move(b.holds);
        out.push_back(std::move(t));
    }
    for (auto& [name, moves] : axes) {
        Track t;
        t.name = name;
        t.axis = true;
        t.moves = std::move(moves);
        out.push_back(std::move(t));
    }
    std::stable_sort(out.begin(), out.end(), [&](const Track& a, const Track& b) {
        if (a.axis != b.axis) return !a.axis;
        return r.Name(a.name) < r.Name(b.name);
    });
    return out;
}

std::vector<Hold> Holds(const Replay& r, std::uint16_t name)
{
    HoldBuilder b;
    for (std::size_t f = 0; f < r.frames.size(); ++f) {
        const Frame& fr = r.frames[f];
        for (std::uint32_t i = 0; i < fr.count; ++i) {
            const Event& e = r.events[fr.first + i];
            if (e.name == name && IsKeyEdge(e.kind)) b.Add(e.kind, static_cast<std::uint32_t>(f));
        }
    }
    b.Finish(static_cast<std::uint32_t>(r.frames.size()));
    return std::move(b.holds);
}

void SetHolds(Replay& r, std::uint16_t name, std::vector<Hold> holds)
{
    holds = Normalize(std::move(holds));
    const std::vector<Hold> before = Holds(r, name);

    // Room for every hold: a release on the frame after the last one held.
    std::uint32_t needed = 0;
    for (const Hold& h : holds) needed = std::max(needed, h.end);
    if (needed > r.frames.size()) {
        const float dt = r.frames.empty() ? (r.fixedHz > 0.f ? 1.f / r.fixedHz : 1.f / 60.f) : r.frames.back().dt;
        r.InsertFrames(r.frames.size(), needed - r.frames.size(), dt);
    }
    const auto frameCount = static_cast<std::uint32_t>(r.frames.size());

    // The pressed value and gamepad flag the key had (keyboard: 1, no gamepad).
    Event press;
    press.name = name;
    press.kind = EventKind::Pressed;
    press.value = 1.f;
    for (const Event& e : r.events)
        if (e.name == name && e.kind == EventKind::Pressed) { press.value = e.value; press.gamepad = e.gamepad; break; }
    Event release = press;
    release.kind = EventKind::Released;
    release.value = 0.f;

    // One pass: every frame keeps its other events, then gets this key's release and press.
    std::vector<Event> events;
    events.reserve(r.events.size() + holds.size() * 2);
    std::size_t nextStart = 0, nextEnd = 0;     // holds are sorted and apart, so their ends are sorted too
    for (std::uint32_t f = 0; f < frameCount; ++f) {
        Frame& fr = r.frames[f];
        const auto first = static_cast<std::uint32_t>(events.size());
        for (std::uint32_t i = 0; i < fr.count; ++i) {
            const Event& e = r.events[fr.first + i];
            if (!(e.name == name && IsKeyEdge(e.kind))) events.push_back(e);
        }
        for (; nextEnd < holds.size() && holds[nextEnd].end == f; ++nextEnd) events.push_back(release);
        for (; nextStart < holds.size() && holds[nextStart].start == f; ++nextStart) events.push_back(press);
        fr.first = first;
        fr.count = static_cast<std::uint32_t>(events.size()) - first;
    }
    r.events = std::move(events);

    // The sync is good up to the first frame whose inputs changed.
    std::uint32_t changed = frameCount;
    const auto firstDiff = [&](const std::vector<Hold>& a, const std::vector<Hold>& b) {
        for (const Hold& h : a)
            if (std::find(b.begin(), b.end(), h) == b.end()) changed = std::min(changed, h.start);
    };
    firstDiff(before, holds);
    firstDiff(holds, before);
    if (changed < frameCount) r.InvalidateSyncFrom(changed + 1);
}

// ---- one frame -------------------------------------------------------------------------------------------------------------
bool HeldAt(const std::vector<Hold>& holds, std::uint32_t frame)
{
    return std::any_of(holds.begin(), holds.end(), [&](const Hold& h) { return frame >= h.start && frame < h.end; });
}

void SetHeld(Replay& r, std::uint16_t name, std::uint32_t frame, bool held)
{
    std::vector<Hold> holds = Holds(r, name);
    if (HeldAt(holds, frame) == held) return;
    if (held) {
        holds.push_back({frame, frame + 1});        // merged with the holds next to it by SetHolds
    } else {
        std::vector<Hold> split;
        for (const Hold& h : holds) {
            if (frame < h.start || frame >= h.end) { split.push_back(h); continue; }
            split.push_back({h.start, frame});      // empty ones are dropped by SetHolds
            split.push_back({frame + 1, h.end});
        }
        holds = std::move(split);
    }
    SetHolds(r, name, std::move(holds));
}

float AxisAt(const Replay& r, std::size_t frame, std::uint16_t name)
{
    if (frame >= r.frames.size()) return 0.f;
    const Frame& f = r.frames[frame];
    float sum = 0.f;
    for (std::uint32_t i = 0; i < f.count; ++i) {
        const Event& e = r.events[f.first + i];
        if (e.name == name && e.kind == EventKind::Axis) sum += e.value;
    }
    return sum;
}

void SetAxis(Replay& r, std::size_t frame, std::uint16_t name, float value)
{
    if (frame >= r.frames.size()) return;
    // Remove this axis's events of the frame (from the back, so the indices stay right), then add one
    std::uint8_t gamepad = 0;
    for (std::uint32_t i = r.frames[frame].count; i-- > 0;) {
        const Event& e = r.events[r.frames[frame].first + i];
        if (e.name != name || e.kind != EventKind::Axis) continue;
        gamepad = e.gamepad;
        r.EraseEvent(frame, i);
    }
    if (value == 0.f) return;
    Event e;
    e.name = name;
    e.kind = EventKind::Axis;
    e.gamepad = gamepad;
    e.value = value;
    e.value2 = r.frames[frame].dt;
    r.InsertEvent(frame, r.frames[frame].count, e);
}

// ---- camera ----------------------------------------------------------------------------------------------------------------
bool CameraAt(const Replay& r, std::size_t frame, float& yaw, float& pitch)
{
    const auto& keys = r.camera;
    if (keys.empty() || frame < keys.front().frame || frame > keys.back().frame) return false;
    const auto next = std::upper_bound(keys.begin(), keys.end(), frame,
                                       [](std::size_t f, const CameraKey& k) { return f < k.frame; });
    const CameraKey& a = *(next - 1);
    if (next == keys.end() || a.frame == frame) {
        yaw = a.yaw;
        pitch = a.pitch;
        return true;
    }
    const CameraKey& b = *next;
    const float t = Ease01(a.ease, static_cast<float>(frame - a.frame) / static_cast<float>(b.frame - a.frame));
    yaw = a.yaw + (b.yaw - a.yaw) * t;
    pitch = a.pitch + (b.pitch - a.pitch) * t;
    return true;
}

bool IsLookAxis(const std::string& name)
{
    return name == "MouseX" || name == "MouseY" || name == "XboxTypeS_RightX" || name == "XboxTypeS_RightY";
}

const CameraKey* FindCameraKey(const Replay& r, std::uint32_t frame)
{
    const auto it = std::lower_bound(r.camera.begin(), r.camera.end(), frame,
                                     [](const CameraKey& k, std::uint32_t f) { return k.frame < f; });
    return it != r.camera.end() && it->frame == frame ? &*it : nullptr;
}

void SetCameraKey(Replay& r, const CameraKey& key)
{
    if (key.frame >= r.frames.size()) return;
    auto it = std::lower_bound(r.camera.begin(), r.camera.end(), key.frame,
                               [](const CameraKey& k, std::uint32_t f) { return k.frame < f; });
    if (it != r.camera.end() && it->frame == key.frame) *it = key;
    else r.camera.insert(it, key);
    // A key changes the camera from the key before it on
    const auto at = std::lower_bound(r.camera.begin(), r.camera.end(), key.frame,
                                     [](const CameraKey& k, std::uint32_t f) { return k.frame < f; });
    r.InvalidateSyncFrom(at == r.camera.begin() ? key.frame : (at - 1)->frame + 1);
}

void EraseCameraKey(Replay& r, std::uint32_t frame)
{
    auto it = std::lower_bound(r.camera.begin(), r.camera.end(), frame,
                               [](const CameraKey& k, std::uint32_t f) { return k.frame < f; });
    if (it == r.camera.end() || it->frame != frame) return;
    const std::uint32_t from = it == r.camera.begin() ? frame : (it - 1)->frame + 1;
    r.camera.erase(it);
    r.InvalidateSyncFrom(from);
}

bool MoveCameraKey(Replay& r, std::uint32_t from, std::uint32_t to)
{
    const CameraKey* k = FindCameraKey(r, from);
    if (!k || to >= r.frames.size()) return false;
    if (from == to) return true;
    CameraKey moved = *k;
    moved.frame = to;
    EraseCameraKey(r, from);
    SetCameraKey(r, moved);
    return true;
}

float UnwrapNear(float deg, float near)
{
    return deg + 360.f * std::round((near - deg) / 360.f);
}

} // namespace tas::timeline
