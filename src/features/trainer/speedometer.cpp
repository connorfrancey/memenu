#include "core/module.h"
#include "game/game.h"
#include "game/player_state.h"
#include "gui.h"

#include <imgui.h>
#include <ME_SDK.hpp>

#include <cmath>
#include <cstdio>
#include <mutex>

namespace {

// Highest value of the last few seconds (the trainer's "top speed"/"top height": 2.75 s, like the game's own meter).
struct Peak {
    float value = 0.f, hitAt = -1e9f;
    bool any = false;

    void Update(float v, float now)
    {
        if (now - hitAt > 2.75f) any = false;
        if (!any || v > value) { value = v; hitAt = now; any = true; }
    }
};

class Speedometer final : public Module {
public:
    Speedometer() : Module("Speedometer", "Trainer", "Position, speed, angles and more in a small window", 70)
    {
        radius.VisibleIf([this] { return showCheckpoint.Get(); });
        setCheckpoint.key = '3';            // the trainer's default
    }

    void OnEnable() override
    {
        std::lock_guard lock(mutex_);
        shown_ = {};
    }

    // Game thread: copy what the window shows.
    void OnTick() override
    {
        auto* pawn = game::Pawn();
        auto* controller = game::Controller();
        auto* world = game::World();

        Values v;
        if (pawn && controller && world) {
            const float now = world->RealTimeSeconds;
            v.valid = true;
            v.x = pawn->Location.X;
            v.y = pawn->Location.Y;
            v.z = pawn->Location.Z;
            v.speed = std::hypot(pawn->Velocity.X, pawn->Velocity.Y);
            topSpeed_.Update(v.speed, now);
            topHeight_.Update(v.z, now);
            v.topSpeed = topSpeed_.value;
            v.topHeight = topHeight_.value;
            v.pitch = static_cast<float>(controller->Rotation.Pitch & 0xFFFF) / 65536.f * 360.f;
            v.yaw = static_cast<float>(controller->Rotation.Yaw & 0xFFFF) / 65536.f * 360.f;
            v.movementState = static_cast<int>(pawn->MovementState.GetValue());
            v.health = pawn->Health;
            v.reactionTime = controller->ReactionTimeEnergy < 100.f ? controller->ReactionTimeEnergy : 100.f;
            v.jumpZ = pawn->LastJumpLocation.Z;
            v.sinceJump = pawn->Location.Z - pawn->LastJumpLocation.Z;
            UpdateCheckpoint(pawn, world);
            v.hasCheckpoint = checkpoint_.set;
            v.checkpointTime = checkpoint_.time;
        } else {
            topSpeed_ = {};
            topHeight_ = {};
        }

        std::lock_guard lock(mutex_);
        shown_ = v;
    }

    void OnRender() override
    {
        Values v;
        {
            std::lock_guard lock(mutex_);
            v = shown_;
        }
        if (!v.valid) return;

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoCollapse;
        if (!titleBar) flags |= ImGuiWindowFlags_NoTitleBar;
        if (!gui::visible) flags |= ImGuiWindowFlags_NoInputs;     // only movable while the menu is open

        ImGui::SetNextWindowPos({60.f, 60.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(opacity);
        if (ImGui::Begin("Speedometer##trainer", nullptr, flags)) {
            const float scale = SpeedScale();
            const char* unit = SpeedUnit();
            if (showPosition) {
                Row("X", "%.2f", v.x);
                Row("Y", "%.2f", v.y);
                Row("Z", "%.2f", v.z);
                Row("Top Z", "%.2f", v.topHeight);
            }
            if (showSpeed) {
                Row("Speed", "%.2f %s", v.speed * scale, unit);
                Row("Top speed", "%.2f %s", v.topSpeed * scale, unit);
            }
            if (showAngles) {
                Row("Pitch", "%.2f", v.pitch);
                Row("Yaw", "%.2f", v.yaw);
            }
            if (showCheckpoint) {
                if (v.hasCheckpoint) Row("Checkpoint", "%.2f s", v.checkpointTime);
                else                 Row("Checkpoint", "%s", "not set");
            }
            if (showState) {
                Row("State", "%d", v.movementState);
                Row("Health", "%d", v.health);
                Row("Reaction", "%.0f", v.reactionTime);
            }
            if (showJump) {
                Row("Jump Z", "%.2f", v.jumpZ);
                Row("Since jump", "%.2f", v.sinceJump);
            }
        }
        ImGui::End();
    }

private:
    struct Values {
        bool valid = false;
        float x = 0.f, y = 0.f, z = 0.f, topHeight = 0.f;
        float speed = 0.f, topSpeed = 0.f, pitch = 0.f, yaw = 0.f;
        int movementState = 0, health = 0;
        float reactionTime = 0.f, jumpZ = 0.f, sinceJump = 0.f;
        bool hasCheckpoint = false;
        float checkpointTime = 0.f;
    };

    // The trainer's checkpoint timer: set a checkpoint, load a state, and the timer runs (in game time) until you get
    // back to the checkpoint.
    struct Checkpoint {
        bool set = false, running = false;
        float x = 0.f, y = 0.f, z = 0.f;
        float time = 0.f, lastWorldTime = 0.f;
        unsigned seenLoads = 0;
    };

    void UpdateCheckpoint(Classes::ATdPlayerPawn* pawn, Classes::AWorldInfo* world)
    {
        Checkpoint& c = checkpoint_;
        const float now = world->TimeSeconds;           // game time: slowed down with the world, like the trainer
        const float dt = now - c.lastWorldTime;
        c.lastWorldTime = now;
        if (!c.set) return;

        if (const unsigned loads = player_state::LoadCount(); loads != c.seenLoads) {
            c.seenLoads = loads;
            c.time = 0.f;
            c.running = true;
            return;
        }
        if (!c.running) return;

        const float dx = pawn->Location.X - c.x, dy = pawn->Location.Y - c.y, dz = pawn->Location.Z - c.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) c.running = false;
        else if (dt > 0.f && dt < 1.f) c.time += dt;
    }

    void SetCheckpoint()
    {
        game::RunOnGameThread([this] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            checkpoint_ = {};
            checkpoint_.set = true;
            checkpoint_.x = pawn->Location.X;
            checkpoint_.y = pawn->Location.Y;
            checkpoint_.z = pawn->Location.Z;
            checkpoint_.seenLoads = player_state::LoadCount();
            if (auto* world = game::World()) checkpoint_.lastWorldTime = world->TimeSeconds;
        });
    }

    float SpeedScale() const
    {
        switch (unit.Get()) {
            case 1:  return 0.036f;         // 1 unit = 1 cm: u/s -> km/h
            case 2:  return 0.01f;          // m/s
            default: return 1.f;
        }
    }
    const char* SpeedUnit() const
    {
        switch (unit.Get()) {
            case 1:  return "km/h";
            case 2:  return "m/s";
            default: return "u/s";
        }
    }

    template <class... Args>
    void Row(const char* label, const char* fmt, Args... args)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, fmt, args...);
        ImGui::TextColored(ToVec4(labelColor.Get()), "%s", label);
        ImGui::SameLine(valueOffset);
        ImGui::TextColored(ToVec4(valueColor.Get()), "%s", buf);
    }

    static ImVec4 ToVec4(const std::array<float, 4>& c) { return {c[0], c[1], c[2], c[3]}; }

    std::mutex mutex_;
    Values shown_;                    // written by OnTick, read by OnRender
    Peak topSpeed_, topHeight_;       // game thread
    Checkpoint checkpoint_;           // game thread

    ctl::Separator itemsTitle{this, "Show"};
    ctl::Checkbox showPosition{this, "Position", true};
    ctl::Checkbox showSpeed{this, "Speed", true};
    ctl::Checkbox showAngles{this, "Pitch / yaw", true};
    ctl::Checkbox showCheckpoint{this, "Checkpoint timer", true,
                                 "Set a checkpoint, then load a state: times how long you take to get back to it"};
    ctl::Checkbox showState{this, "State / health / reaction time", false};
    ctl::Checkbox showJump{this, "Jump height", false};
    ctl::Combo unit{this, "Speed unit", {"u/s", "km/h", "m/s"}};

    ctl::Separator checkpointTitle{this, "Checkpoint"};
    ctl::Button setCheckpoint{this, "Set checkpoint here", [this] { SetCheckpoint(); }};
    ctl::Button clearCheckpoint{this, "Clear checkpoint", [this] { game::RunOnGameThread([this] { checkpoint_ = {}; }); }};
    ctl::Slider radius{this, "Stop radius", 50.f, 1.f, 500.f, "%.0f", 10.f, "The timer stops this close to the checkpoint"};

    ctl::Separator lookTitle{this, "Look"};
    ctl::Checkbox titleBar{this, "Title bar", false};
    ctl::Slider opacity{this, "Opacity", 0.6f, 0.f, 1.f, "%.2f", 0.05f};
    ctl::Slider valueOffset{this, "Value column", 90.f, 40.f, 200.f, "%.0f", 5.f};
    ctl::ColorPick labelColor{this, "Label color", {0.75f, 0.75f, 0.78f, 1.f}};
    ctl::ColorPick valueColor{this, "Value color", {1.f, 1.f, 1.f, 1.f}};
};
MEM_MODULE(Speedometer)

} // namespace
