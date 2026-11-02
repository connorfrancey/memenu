// Collision view: trigger, kill, checkpoint, blocking and movement volumes drawn as solid, translucent geometry.
// The volumes are collected from the SDK on the game thread (game/debug_world.*) and drawn by the Direct3D 9 renderer in
// the EndScene / StretchRect hooks (render/debug_renderer.*). This module only owns the settings and wires the two together.
#include "core/module.h"
#include "game/debug_world.h"
#include "game/game.h"
#include "render/debug_renderer.h"

#include <imgui.h>

#include <array>
#include <initializer_list>
#include <cstdio>

namespace {

ImU32 ToImColor(std::uint32_t argb)
{
    return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255);
}

// One kind of volume: whether it is shown and in which color. The two controls are saved (and keep their keys) as
// before, but the panel draws all layers together in a compact grid (CollisionView::DrawLayers).
struct Layer {
    Layer(Module* m, const char* showName, const char* colorName, const char* shortName, bool shown,
          std::array<float, 4> rgba, const char* tip = "")
        : show(m, showName, shown, tip), color(m, colorName, rgba), label(shortName), tooltip(tip)
    {
        show.VisibleIf([] { return false; });
        color.VisibleIf([] { return false; });
    }

    ctl::Checkbox show;
    ctl::ColorPick color;
    const char* label;
    const char* tooltip;
};

class CollisionView final : public Module {
public:
    CollisionView() : Module("Collision View", "Debug", "Draw collision and trigger volumes as solid geometry")
    {
        const auto in = [](ctl::Section& section, std::initializer_list<ctl::Setting*> controls) {
            for (ctl::Setting* c : controls) c->VisibleIf([&section] { return section.Get(); });
        };
        in(showSection, {&layerGrid});
        in(displaySection, {&style, &opacity, &depth, &maxDistance, &labels});
        in(cacheSection, {&cacheStatus});
        in(advancedSection, {&updateRate, &fovOffset, &aspectOverride, &useCache, &scanCached, &statsLine, &tools});
    }

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
        if (statsLine) DrawStatsLine();
    }

private:
    // Top left of the screen: what the renderer drew and how (for checking the renderer, off by default).
    static void DrawStatsLine()
    {
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

    // Every kind of volume in two columns: shown or not, and its color as a small swatch next to it.
    void DrawLayers(float)
    {
        Layer* const all[] = {&triggers, &killZones, &checkpoints, &blocking, &movement, &otherVolumes, &cylinders};
        if (!ImGui::BeginTable("##layers", 2, ImGuiTableFlags_SizingStretchSame)) return;
        for (Layer* layer : all) {
            ImGui::TableNextColumn();
            ImGui::PushID(layer->label);
            auto rgba = layer->color.Get();
            if (ImGui::ColorEdit3("##color", rgba.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                layer->color.Set(rgba);
            ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
            bool shown = layer->show;
            if (ImGui::Checkbox(layer->label, &shown)) layer->show.Set(shown);
            if (layer->tooltip[0]) ImGui::SetItemTooltip("%s", layer->tooltip);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // This level's cache in one line, the bake's progress, and the buttons for both.
    static void DrawCacheStatus(float width)
    {
        const auto info = debugworld::GetInfo();
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        ImGui::TextDisabled("%s: %zu volumes, %s", info.map.empty() ? "No level" : info.map.c_str(), info.shapes,
                            info.cacheDirty ? "not saved yet" : info.fromDisk ? "cached" : "this session");
        ImGui::SetItemTooltip("%zu scans, the last took %.1f ms", info.scans, info.lastScanMs);

        if (info.baking) {
            char overlay[48];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu checkpoints", info.bakeDone, info.bakeTotal);
            ImGui::ProgressBar(info.bakeTotal ? static_cast<float>(info.bakeDone) / static_cast<float>(info.bakeTotal)
                                              : 0.f,
                               {width, 0}, overlay);
        }
        if (!info.bakeStatus.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextDisabled("%s%s", info.baking ? "" : "Last bake: ", info.bakeStatus.c_str());
            if (info.bakeFailed)
                ImGui::TextColored({1.f, 0.6f, 0.3f, 1.f}, "%zu checkpoint(s) did not load and were skipped", info.bakeFailed);
            ImGui::PopTextWrapPos();
        }

        if (info.baking) {
            if (ImGui::Button("Cancel baking", {width, 0})) debugworld::CancelBake();
            ImGui::SetItemTooltip("Saves this checkpoint and takes you back to the level you were in");
        } else {
            if (ImGui::Button("Bake missing", {half, 0})) debugworld::BakeAllLevels(true);
            ImGui::SetItemTooltip("Loads every checkpoint of each level not baked yet, in story order, and caches its "
                                  "volumes. A few seconds per checkpoint; you are taken back here afterwards");
            ImGui::SameLine();
            if (ImGui::Button("Rebake all", {half, 0})) debugworld::BakeAllLevels(false);
            ImGui::SetItemTooltip("The same for every level, baked or not");
        }

        if (ImGui::Button("Rescan", {half, 0})) debugworld::RescanNow();
        ImGui::SetItemTooltip("Look for volumes in this level again now");
        ImGui::SameLine();
        if (ImGui::Button(confirmClear_ ? "Sure?" : "Clear level", {half, 0})) {
            if (confirmClear_) debugworld::ClearCache();
            confirmClear_ = !confirmClear_;
        }
        if (!ImGui::IsItemHovered()) confirmClear_ = false;
        ImGui::SetItemTooltip("Forget this level's volumes (and its cache file), then scan it again");
    }

    static void DrawTools(float width)
    {
        if (ImGui::Button("Rebuild now", {width, 0})) debugworld::RequestRebuild();
        ImGui::SetItemTooltip("Rebuild the drawn scene from the remembered volumes");
    }

    static inline bool confirmClear_ = false;

    // ---- controls (listed in the order they appear in the menu) --------------------------------------------------------
    ctl::Section showSection{this, "Show", true};
    ctl::Custom layerGrid{this, [this](float w) { DrawLayers(w); }};
    Layer triggers{this, "Triggers", "Triggers color", "Triggers", true, {1.00f, 0.55f, 0.10f, 1.f}};
    Layer killZones{this, "Kill zones", "Kill zones color", "Kill zones", true, {0.92f, 0.16f, 0.16f, 1.f}};
    Layer checkpoints{this, "Checkpoints", "Checkpoints color", "Checkpoints", true, {0.20f, 0.85f, 0.35f, 1.f}};
    Layer blocking{this, "Blocking volumes", "Blocking color", "Blocking", true, {0.25f, 0.50f, 1.00f, 1.f}};
    Layer movement{this, "Movement volumes", "Movement color", "Movement", false, {0.65f, 0.35f, 0.95f, 1.f},
                   "Ladders, ziplines, swing/balance/ledge volumes, water, gravity, fall height"};
    Layer otherVolumes{this, "Other volumes", "Other volumes color", "Other", false, {0.20f, 0.80f, 0.80f, 1.f},
                       "Every other kind of volume: post process, reverb, fog, streaming..."};
    Layer cylinders{this, "Cylinders", "Cylinders color", "Cylinders", true, {0.95f, 0.85f, 0.25f, 1.f},
                    "Cylinder collision of pawns and other moving actors (looked up live)"};

    ctl::Section displaySection{this, "Display", true};
    ctl::Combo style{this, "Style", {"Solid", "Solid + outline", "Wireframe"}, 1};
    ctl::Slider opacity{this, "Opacity", 0.55f, 0.1f, 1.f, "%.2f", 0.05f, "1 = fully opaque"};
    ctl::Combo depth{this, "Depth", {"Hidden by the world", "Always on top"}, 0,
                     "Hidden by the world: drawn inside the game's frame, so walls and props hide the volumes behind "
                     "them. Always on top: drawn over the finished frame, nothing hides them"};
    ctl::Slider maxDistance{this, "Max distance", 6000.f, 500.f, 40000.f, "%.0f", 500.f,
                            "Volumes farther away than this are not drawn"};
    ctl::Checkbox labels{this, "Labels", false, "Class names of triggers, kill zones, checkpoints and volumes"};

    ctl::Section cacheSection{this, "Cache & baking", true};
    ctl::Custom cacheStatus{this, [](float w) { DrawCacheStatus(w); }};

    ctl::Section advancedSection{this, "Advanced", false};
    ctl::Slider updateRate{this, "Update rate", 3.f, 1.f, 20.f, "%.0f Hz", 1.f,
                           "How often the scene is rebuilt from the remembered volumes"};
    ctl::Slider fovOffset{this, "FOV offset", 0.f, -30.f, 30.f, "%.1f", 0.5f,
                          "Added to the camera FOV. Use it if the shapes do not line up with the world"};
    ctl::Slider aspectOverride{this, "Aspect override", 0.f, 0.f, 3.f, "%.2f", 0.05f, "0 = taken from the viewport"};
    ctl::Checkbox useCache{this, "Use level cache", true,
                           "Volumes never change in a level, so they are remembered per level in the 'cache' "
                           "folder next to the DLL and are there at once the next time the level loads"};
    ctl::Checkbox scanCached{this, "Scan cached levels", true,
                             "Keep looking for volumes that stream in after the cache was written. "
                             "Turn off once a level is baked"};
    ctl::Checkbox statsLine{this, "Stats line", false, "What the renderer drew, top left of the screen"};
    ctl::Custom tools{this, [](float w) { DrawTools(w); }};
};
MEM_MODULE(CollisionView)

} // namespace
