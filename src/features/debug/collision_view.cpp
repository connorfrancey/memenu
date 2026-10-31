// Collision view: trigger, kill, checkpoint, blocking and movement volumes drawn as solid, translucent geometry.
// The volumes are collected from the SDK on the game thread (game/debug_world.*) and drawn by the Direct3D 9 renderer in
// the EndScene / StretchRect hooks (render/debug_renderer.*). This module only owns the settings and wires the two together.
#include "core/module.h"
#include "game/debug_world.h"
#include "game/game.h"
#include "render/debug_renderer.h"

#include <imgui.h>

#include <array>
#include <cstdio>

namespace {

ImU32 ToImColor(std::uint32_t argb)
{
    return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255);
}

// One kind of volume: whether it is shown and in which color.
struct Layer {
    Layer(Module* m, const char* showName, const char* colorName, bool shown, std::array<float, 4> rgba, const char* tip = "")
        : show(m, showName, shown, tip), color(m, colorName, rgba) {}

    ctl::Checkbox show;
    ctl::ColorPick color;
};

class CollisionView final : public Module {
public:
    CollisionView() : Module("Collision View", "Debug", "Draw collision and trigger volumes as solid geometry") {}

    void OnEnable() override
    {
        debugrender::SetActive(true);
        game::SetCameraTracking(true);
        debugworld::RequestRebuild();
    }

    void OnDisable() override
    {
        debugrender::SetActive(false);
        debugrender::SetScene(nullptr);
        game::SetCameraTracking(false);
    }

    void OnTick() override
    {
        debugrender::SetCamera(game::GetCamera());
        debugrender::SetEngineNear(game::EngineNear());
        debugrender::SetStyle(RenderStyle());
        debugworld::Update(WorldConfig());
    }

    void OnRender() override
    {
        if (labels) DrawLabels();

        const auto st = debugrender::GetStats();
        const char* how = st.path == debugrender::Path::InScene ? "in-scene (depth)" :
                          st.path == debugrender::Path::Overlay ? "overlay" : "-";
        char line[200];
        std::snprintf(line, sizeof line, "Collision view: %zu tris, %zu lines, %zu shapes, build %.1f ms, %s%s%s",
                      st.triangles, st.lines, st.shapes, st.buildMs, how,
                      st.overlayFallback ? " [FALLBACK: scene pass not found]" : "",
                      st.drawn ? "" : " (not drawing yet)");
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText({11.f, 65.f}, IM_COL32(0, 0, 0, 200), line);
        dl->AddText({10.f, 64.f}, st.overlayFallback ? IM_COL32(255, 190, 90, 255) : IM_COL32(200, 200, 210, 255), line);
    }

private:
    debugworld::Config WorldConfig() const
    {
        using debugworld::Category;
        debugworld::Config c;
        c.style = static_cast<debugworld::DrawStyle>(style.Get());
        c.maxDistance = maxDistance;
        c.updateHz = updateRate;
        c.labels = labels;
        c.useCache = useCache;
        c.scanCached = scanCached;

        const auto set = [&c](Category cat, const Layer& layer) {
            const auto i = static_cast<std::size_t>(cat);
            const auto& rgba = layer.color.Get();
            c.enabled[i] = layer.show;
            c.colors[i] = rmesh::PackColor(rgba[0], rgba[1], rgba[2], 1.f);
        };
        set(Category::Trigger, triggers);
        set(Category::Kill, killZones);
        set(Category::Checkpoint, checkpoints);
        set(Category::Blocking, blocking);
        set(Category::Movement, movement);
        set(Category::Volume, otherVolumes);
        set(Category::Cylinder, cylinders);
        return c;
    }

    debugrender::Style RenderStyle() const
    {
        debugrender::Style s;
        s.opacity = opacity;
        s.depth = depth.Get() == 0 ? debugrender::Depth::Scene : debugrender::Depth::Overlay;
        s.fovOffset = fovOffset;
        s.aspect = aspectOverride;
        return s;
    }

    // Labels are drawn with ImGui (render thread) at the projected position of each trigger/volume.
    static void DrawLabels()
    {
        const auto scene = debugrender::CurrentScene();
        if (!scene) return;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        for (const auto& label : scene->labels) {
            float x = 0.f, y = 0.f;
            if (!debugrender::WorldToScreen(label.pos, x, y)) continue;
            dl->AddText({x + 1.f, y + 1.f}, IM_COL32(0, 0, 0, 220), label.text.c_str());
            dl->AddText({x, y}, ToImColor(label.color), label.text.c_str());
        }
    }

    static void DrawCacheInfo(float)
    {
        const auto info = debugworld::GetInfo();
        ImGui::TextDisabled("Level: %s", info.map.empty() ? "-" : info.map.c_str());
        ImGui::TextDisabled("%zu remembered volumes (%s%s), %zu scans, last %.1f ms", info.shapes,
                            info.fromDisk ? "from disk" : "this session", info.cacheDirty ? ", not saved yet" : "", info.scans,
                            info.lastScanMs);
        if (info.baking || !info.bakeStatus.empty()) ImGui::TextWrapped("Baking: %s", info.bakeStatus.c_str());
    }

    // ---- controls (listed in the order they appear in the menu) --------------------------------------------------------
    ctl::Separator displayHeader{this, "Display"};
    ctl::Combo style{this, "Style", {"Solid", "Solid + outline", "Wireframe"}, 1};
    ctl::Slider opacity{this, "Opacity", 0.55f, 0.1f, 1.f, "%.2f", 0.05f, "1 = fully opaque"};
    ctl::Combo depth{this, "Depth", {"Scene depth (hidden by the world)", "Overlay (always on top)"}, 0,
                     "Scene depth: drawn inside the game's frame, so walls and props hide the volumes behind "
                     "them. Overlay: drawn over the finished frame, nothing hides them"};
    ctl::Slider maxDistance{this, "Max distance", 6000.f, 500.f, 40000.f, "%.0f", 500.f,
                            "Volumes farther away than this are not drawn"};
    ctl::Slider updateRate{this, "Update rate", 3.f, 1.f, 20.f, "%.0f Hz", 1.f,
                           "How often the scene is rebuilt from the remembered volumes"};
    ctl::Checkbox labels{this, "Labels", false, "Class names of triggers, kill zones, checkpoints and volumes"};

    ctl::Separator showHeader{this, "Show"};
    Layer triggers{this, "Triggers", "Triggers color", true, {1.00f, 0.55f, 0.10f, 1.f}};
    Layer killZones{this, "Kill zones", "Kill zones color", true, {0.92f, 0.16f, 0.16f, 1.f}};
    Layer checkpoints{this, "Checkpoints", "Checkpoints color", true, {0.20f, 0.85f, 0.35f, 1.f}};
    Layer blocking{this, "Blocking volumes", "Blocking color", true, {0.25f, 0.50f, 1.00f, 1.f}};
    Layer movement{this, "Movement volumes", "Movement color", false, {0.65f, 0.35f, 0.95f, 1.f},
                   "Ladders, ziplines, swing/balance/ledge volumes, water, gravity, fall height"};
    Layer otherVolumes{this, "Other volumes", "Other volumes color", false, {0.20f, 0.80f, 0.80f, 1.f}};
    Layer cylinders{this, "Cylinders", "Cylinders color", true, {0.95f, 0.85f, 0.25f, 1.f},
                    "Cylinder collision of pawns and other moving actors (looked up live)"};

    ctl::Separator cacheHeader{this, "Level cache"};
    ctl::Checkbox useCache{this, "Use level cache", true,
                           "Volumes never change in a level, so they are remembered per level in the 'cache' "
                           "folder next to the DLL and are there at once the next time the level loads"};
    ctl::Checkbox scanCached{this, "Scan cached levels", true,
                             "Keep looking for volumes that stream in after the cache was written. "
                             "Turn off once a level is baked"};
    ctl::Custom cacheInfo{this, [](float w) { DrawCacheInfo(w); }};
    ctl::Button rescan{this, "Rescan now", [] { debugworld::RescanNow(); }};
    ctl::Button clearCache{this, "Clear this level's cache", [] { debugworld::ClearCache(); }};
    ctl::Button bakeAll{this, "Bake all levels", [] { debugworld::BakeAllLevels(); },
                        "Loads every level and checkpoint in turn and writes their volumes to the cache. "
                        "Takes a few minutes"};
    ctl::Button cancelBake{this, "Cancel baking", [] { debugworld::CancelBake(); }};

    ctl::Separator calibrationHeader{this, "Calibration"};
    ctl::Slider fovOffset{this, "FOV offset", 0.f, -30.f, 30.f, "%.1f", 0.5f,
                          "Added to the camera FOV. Use it if the shapes do not line up with the world"};
    ctl::Slider aspectOverride{this, "Aspect override", 0.f, 0.f, 3.f, "%.2f", 0.05f, "0 = taken from the viewport"};
    ctl::Button rebuild{this, "Rebuild now", [] { debugworld::RequestRebuild(); }};
};
MEM_MODULE(CollisionView)

} // namespace
