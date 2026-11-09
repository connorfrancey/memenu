// Frame Rate Limit: the game's frame rate cap (UGameEngine.bSmoothFrameRate / MaxSmoothedFrameRate, 62 fps) set to
// another value, or off. Off puts back exactly what the engine had, so normal play is at the game's own 62.
//
// The game's movement depends on the frame rate (jump heights, kick and wallrun timing), so runs at another cap play
// differently: speedruns are timed at 62. A TAS session sets every frame's length itself and ignores this.
#include "core/module.h"
#include "game/game.h"
#include "game/object_ref.h"
#include "game/uobject.h"

#include <ME_SDK.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cmath>

namespace {

constexpr float kGameCap = 62.f;

Classes::UGameEngine* Engine()
{
    static game::ObjectRef<Classes::UGameEngine> engine{"Game engine (frame rate)"};
    return engine.Find([](Classes::UObject* o) {
        return game::IsA(o, Classes::UGameEngine::StaticClass()) && !game::IsDefaultObject(o);
    });
}

class FrameRate final : public Module {
public:
    FrameRate()
        : Module("Frame Rate Limit", "Visual",
                 "Change the game's 62 fps cap. Off: the game's own 62. Speedruns are at 62: movement depends on it", 15)
    {
        cap.VisibleIf([this] { return !uncapped.Get(); });
    }

    void OnTick() override
    {
        auto* engine = Engine();
        if (!engine) return;
        if (!saved_) {      // What to put back when switched off
            savedSmooth_ = engine->bSmoothFrameRate != 0;
            savedMax_ = engine->MaxSmoothedFrameRate;
            saved_ = true;
        }
        const bool smooth = !uncapped.Get();
        const float max = cap.Get();
        if ((engine->bSmoothFrameRate != 0) != smooth) engine->bSmoothFrameRate = smooth ? 1 : 0;
        if (smooth && std::fabs(engine->MaxSmoothedFrameRate - max) > 0.01f) {
            engine->MaxSmoothedFrameRate = max;
            if (engine->MinSmoothedFrameRate > max) engine->MinSmoothedFrameRate = max;
        }
    }

    void OnDisable() override
    {
        // May be on another thread: the engine is touched on the game thread
        game::RunOnGameThread([this] {
            if (!saved_) return;
            if (auto* engine = Engine()) {
                engine->bSmoothFrameRate = savedSmooth_ ? 1 : 0;
                engine->MaxSmoothedFrameRate = savedMax_;
                spdlog::info("Frame Rate Limit: off, the game's cap is back ({}, {:.0f} fps)",
                             savedSmooth_ ? "on" : "off", savedMax_);
            }
            saved_ = false;
        });
    }

private:
    void DrawNote(float width)
    {
        if (!uncapped.Get() && std::fabs(cap.Get() - kGameCap) < 0.01f) return;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Not 62: the game moves differently at other frame rates. "
                                                    "Not for speedruns");
        ImGui::PopTextWrapPos();
    }

    // Game thread
    bool saved_ = false;
    bool savedSmooth_ = true;
    float savedMax_ = kGameCap;

    ctl::Slider cap{this, "Cap", kGameCap, 20.f, 300.f, "%.0f fps", 1.f, "The most frames a second the game draws"};
    ctl::Checkbox uncapped{this, "Uncapped", false, "As many frames as the PC can draw"};
    ctl::Button reset{this, "Back to 62", [this] {
        cap.Set(kGameCap);
        uncapped.Set(false);
    }};
    ctl::Custom note{this, [this](float w) { DrawNote(w); }};
};
MEM_MODULE(FrameRate)

} // namespace
