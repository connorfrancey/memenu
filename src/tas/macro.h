#pragma once
#include "tas/replay.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Writes a macro (a replay that starts "now", from wherever the player is) as a script of inputs instead of recording
// it. The result is an ordinary Replay: play it with tas::WithReplay + tas::Play, or save it as a .merp.
//
//     tas::Macro m;                       // 60 frames per second
//     m.Wait(5)                           // 5 frames without new input
//      .Press("W")                        // on frame 5
//      .Wait(30)
//      .Hold("SpaceBar", 2)               // pressed on frame 35, released on frame 37
//      .Wait(28)
//      .Release("W");                     // frame 65
//     tas::Replay r = m.Build();          // ends with one more frame; keys still held are released there
//
// Inputs land on the current frame; Wait() moves on. Key names are the engine's ("W", "SpaceBar", "LeftMouseButton",
// axes "MouseX"/"MouseY"), so a macro follows the player's own bindings only if it is written with their keys.
namespace tas {

class Macro {
public:
    explicit Macro(float hz = 60.f) : hz_(hz > 0.f ? hz : 60.f) { frames_.emplace_back(); }

    Macro& Wait(std::size_t frames)
    {
        for (std::size_t i = 0; i < frames; ++i) frames_.emplace_back();
        return *this;
    }

    Macro& Press(const std::string& key)
    {
        Add(key, EventKind::Pressed, 1.f, 0.f);
        if (std::find(held_.begin(), held_.end(), key) == held_.end()) held_.push_back(key);
        return *this;
    }

    Macro& Release(const std::string& key)
    {
        Add(key, EventKind::Released, 0.f, 0.f);
        held_.erase(std::remove(held_.begin(), held_.end(), key), held_.end());
        return *this;
    }

    // Pressed now, released `frames` frames later (where the script continues).
    Macro& Hold(const std::string& key, std::size_t frames = 1) { return Press(key).Wait(frames).Release(key); }

    // Mouse / stick movement on the current frame ("MouseX", "MouseY", ...).
    Macro& Axis(const std::string& axis, float delta)
    {
        Add(axis, EventKind::Axis, delta, 1.f / hz_);
        return *this;
    }

    Macro& Describe(std::string text)
    {
        description_ = std::move(text);
        return *this;
    }

    std::size_t Frame() const { return frames_.size() - 1; }   // the frame inputs go to now
    float Hz() const { return hz_; }

    Replay Build() const
    {
        Replay r;
        r.start = StartKind::Now;
        r.fixedHz = hz_;
        r.description = description_;
        r.botName = "memenu macro";
        const float dt = 1.f / hz_;
        for (const auto& frame : frames_) {
            r.BeginFrame(dt);
            for (const auto& in : frame) r.AddEvent(MakeEvent(r, in));
        }
        if (!held_.empty()) {               // Nothing stays pressed after the macro
            r.BeginFrame(dt);
            for (const auto& key : held_) r.AddEvent(MakeEvent(r, {key, EventKind::Released, 0.f, 0.f}));
        }
        r.syncValidUntil = 0;               // Written, not recorded: nothing to compare with
        return r;
    }

private:
    struct Input {
        std::string name;
        EventKind kind;
        float value, value2;
    };

    void Add(const std::string& name, EventKind kind, float value, float value2)
    {
        frames_.back().push_back({name, kind, value, value2});
    }

    static Event MakeEvent(Replay& r, const Input& in)
    {
        Event e;
        e.name = r.NameIndex(in.name);
        e.kind = in.kind;
        e.value = in.value;
        e.value2 = in.value2;
        return e;
    }

    float hz_;
    std::string description_;
    std::vector<std::vector<Input>> frames_;   // inputs per frame
    std::vector<std::string> held_;            // pressed and not released yet
};

} // namespace tas
