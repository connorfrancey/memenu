// Collision view: trigger, kill, checkpoint, blocking and movement volumes drawn as solid, translucent geometry, and the
// actors placed with them (player starts, pickups, AI-only volumes and areas, navigation points).
// The volumes are collected from the SDK on the game thread (game/debug_world.*) and drawn by the Direct3D 9 renderer in
// the EndScene / StretchRect hooks (render/debug_renderer.*). This module only owns the settings and wires the two together.
#include "core/module.h"
#include "game/debug_world.h"
#include "game/game.h"
#include "render/debug_renderer.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <cstdio>
#include <string>

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
        in(showSection, {&layerGrid, &filterBox});
        in(displaySection, {&style, &opacity, &depth, &maxDistance, &fade, &labels});
        in(inspectSection, {&lookAt, &highlight, &insideList, &ownCylinder, &inspectInfo});
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
        debugrender::SetDynamic({});
        game::SetCameraTracking(false);
    }

    void OnTick() override
    {
        debugrender::SetCamera(game::GetCamera());
        debugrender::SetEngineNear(game::EngineNear());
        debugrender::SetStyle(RenderStyle());
        const auto cfg = WorldConfig();
        if (cfg.filter != lastFilter_) {
            lastFilter_ = cfg.filter;
            debugworld::RequestRebuild();
        }
        debugworld::Update(cfg);
    }

    void OnRender() override
    {
        info_ = debugworld::GetInfo();
        if (labels) DrawLabels();
        if (statsLine) DrawStatsLine();
        if (lookAt) DrawLookAt();
        if (insideList) DrawInsideList();
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

    // ---- Inspect ---------------------------------------------------------------------------------------------------
    // Every layer, in the order of debugworld::Category
    std::array<const Layer*, debugworld::kCategoryCount> Layers() const
    {
        return {&triggers, &killZones, &checkpoints, &blocking, &movement, &otherVolumes, &cylinders,
                &spawns, &pickups, &aiOnly, &navigation};
    }

    const Layer& LayerOf(int category) const
    {
        return *Layers()[static_cast<std::size_t>(std::clamp(category, 0, debugworld::kCategoryCount - 1))];
    }

    static void ShadowText(ImDrawList* dl, ImVec2 at, ImU32 color, const char* text)
    {
        dl->AddText({at.x + 1.f, at.y + 1.f}, IM_COL32(0, 0, 0, 220), text);
        dl->AddText(at, color, text);
    }

    // Under the crosshair: what the view ray hits first.
    void DrawLookAt() const
    {
        const auto& look = info_.look;
        if (!look.valid) return;
        const auto& rgba = LayerOf(look.category).color.Get();
        const ImU32 color = ImGui::ColorConvertFloat4ToU32({rgba[0], rgba[1], rgba[2], 1.f});
        char line2[96];
        std::snprintf(line2, sizeof line2, "%s  |  %.0f away  |  %.0f across", debugworld::CategoryName(look.category),
                      look.distance, look.size);
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const char* name = look.name.empty() ? "(unnamed)" : look.name.c_str();
        const float w1 = ImGui::CalcTextSize(name).x, w2 = ImGui::CalcTextSize(line2).x;
        const float y = screen.y * 0.5f + 28.f;
        ShadowText(dl, {screen.x * 0.5f - w1 * 0.5f, y}, color, name);
        ShadowText(dl, {screen.x * 0.5f - w2 * 0.5f, y + ImGui::GetTextLineHeight()}, IM_COL32(220, 220, 225, 255), line2);
        if (!look.behind.empty()) {
            std::string more = "behind: " + look.behind[0].first;
            if (look.behind.size() > 1) more += " +" + std::to_string(look.behind.size() - 1);
            const float w3 = ImGui::CalcTextSize(more.c_str()).x;
            ShadowText(dl, {screen.x * 0.5f - w3 * 0.5f, y + 2.f * ImGui::GetTextLineHeight()}, IM_COL32(160, 160, 170, 255),
                       more.c_str());
        }
    }

    // Top left: the volumes the player is in.
    void DrawInsideList() const
    {
        if (info_.inside.empty()) return;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float y = statsLine ? 86.f : 64.f;
        ShadowText(dl, {10.f, y}, IM_COL32(200, 200, 210, 255), "Inside:");
        for (const auto& [name, category] : info_.inside) {
            y += ImGui::GetTextLineHeight();
            const auto& rgba = LayerOf(category).color.Get();
            char line[160];
            std::snprintf(line, sizeof line, "%s  (%s)", name.empty() ? "(unnamed)" : name.c_str(),
                          debugworld::CategoryName(category));
            ShadowText(dl, {18.f, y}, ImGui::ColorConvertFloat4ToU32({rgba[0], rgba[1], rgba[2], 1.f}), line);
        }
    }

    // In the panel: the same, for when the HUD text is off.
    void DrawInspectInfo(float)
    {
        const auto& look = info_.look;
        if (look.valid) {
            ImGui::TextDisabled("Looking at");
            ImGui::SameLine();
            ImGui::TextUnformatted(look.name.empty() ? "(unnamed)" : look.name.c_str());
            ImGui::TextDisabled("%s, %.0f away, %.0f across", debugworld::CategoryName(look.category), look.distance, look.size);
            ImGui::TextDisabled("hit at %.0f, %.0f, %.0f", look.x, look.y, look.z);
            if (ImGui::IsItemClicked()) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.0f %.0f %.0f", look.x, look.y, look.z);
                ImGui::SetClipboardText(buf);
            }
            ImGui::SetItemTooltip("Click to copy");
            for (const auto& [name, at] : look.behind) ImGui::TextDisabled("  behind it: %s (%.0f)", name.c_str(), at);
        } else ImGui::TextDisabled("Not looking at a volume");
        if (info_.inside.empty()) ImGui::TextDisabled("Not inside any volume");
        else ImGui::TextDisabled("Inside %zu volume%s", info_.inside.size(), info_.inside.size() == 1 ? "" : "s");

        // The closest volumes: click one to turn toward it
        if (info_.nearby.empty()) return;
        ImGui::Spacing();
        ImGui::TextDisabled("Nearby (click to face it)");
        if (!ImGui::BeginTable("##nearby", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return;
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("dist", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000").x);
        for (std::size_t i = 0; i < info_.nearby.size(); ++i) {
            const auto& n = info_.nearby[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            const auto& rgba = LayerOf(n.category).color.Get();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{rgba[0], rgba[1], rgba[2], 1.f});
            if (ImGui::Selectable(n.name.empty() ? "(unnamed)" : n.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                debugworld::FaceTowards(n.x, n.y, n.z);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s at %.0f, %.0f, %.0f", debugworld::CategoryName(n.category), n.x, n.y, n.z);
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (n.distance <= 0.f) ImGui::TextDisabled("in");
            else ImGui::TextDisabled("%.0f", n.distance);
        }
        ImGui::EndTable();
    }

    void DrawFilter(float width)
    {
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputTextWithHint("##filter", "Filter by name (e.g. kill)", filter_, sizeof filter_)) MarkDirty();
        ImGui::SetItemTooltip("Only volumes whose name contains this. Names are the level's actor names: "
                              "TdKillVolume, TdTriggerVolume, BlockingVolume, ...");
    }

    std::string Filter() const
    {
        std::string f = filter_;
        f.erase(0, f.find_first_not_of(' '));
        f.erase(f.find_last_not_of(' ') + 1);
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return f;
    }

    debugworld::Config WorldConfig() const
    {
        using debugworld::Category;
        debugworld::Config c;
        c.fade = fade;
        c.filter = Filter();
        c.ownCylinder = ownCylinder;
        c.inspect = lookAt || highlight || insideList || expanded;
        c.highlight = highlight;
        c.nearby = expanded && inspectSection;
        c.style = static_cast<debugworld::DrawStyle>(style.Get());
        c.maxDistance = maxDistance;
        c.updateHz = updateRate;
        c.labels = labels;
        c.useCache = useCache;
        c.scanCached = scanCached;

        const auto layers = Layers();
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const auto& rgba = layers[i]->color.Get();
            c.enabled[i] = layers[i]->show;
            c.colors[i] = rmesh::PackColor(rgba[0], rgba[1], rgba[2], 1.f);
        }
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
        const auto all = Layers();
        if (!ImGui::BeginTable("##layers", 2, ImGuiTableFlags_SizingStretchSame)) return;
        for (std::size_t i = 0; i < all.size(); ++i) {
            Layer* layer = const_cast<Layer*>(all[i]);      // In the order of debugworld::Category
            ImGui::TableNextColumn();
            ImGui::PushID(layer->label);
            auto rgba = layer->color.Get();
            if (ImGui::ColorEdit3("##color", rgba.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                layer->color.Set(rgba);
            ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
            bool shown = layer->show;
            if (ImGui::Checkbox(layer->label, &shown)) layer->show.Set(shown);
            if (layer->tooltip[0]) ImGui::SetItemTooltip("%s", layer->tooltip);
            // How many are drawn now, of how many known in this level
            if (info_.remembered[i] > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%zu/%zu", info_.drawn[i], info_.remembered[i]);
                ImGui::SetItemTooltip("%zu drawn (in range and matching the filter) of %zu %s", info_.drawn[i],
                                      info_.remembered[i],
                                      i == static_cast<std::size_t>(debugworld::Category::Cylinder) ? "found nearby"
                                                                                                    : "in this level");
            }
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
    debugworld::Info info_;             // Render thread: this frame's copy
    char filter_[64] = {};
    std::string lastFilter_;            // Game thread: the filter the scene was built with

    // ---- controls (listed in the order they appear in the menu) --------------------------------------------------------
    ctl::Section showSection{this, "Show", true};
    ctl::Custom layerGrid{this, [this](float w) { DrawLayers(w); }};
    Layer triggers{this, "Triggers", "Triggers color", "Triggers", true, {1.00f, 0.55f, 0.10f, 1.f}};
    Layer killZones{this, "Kill zones", "Kill zones color", "Kill zones", true, {0.92f, 0.16f, 0.16f, 1.f}};
    Layer checkpoints{this, "Checkpoints", "Checkpoints color", "Checkpoints", true, {0.20f, 0.85f, 0.35f, 1.f}};
    Layer blocking{this, "Blocking volumes", "Blocking color", "Blocking", true, {0.25f, 0.50f, 1.00f, 1.f}};
    Layer movement{this, "Movement volumes", "Movement color", "Movement", false, {0.65f, 0.35f, 0.95f, 1.f},
                   "Ladders, ziplines, swing/balance/ledge volumes, water, gravity, fall height, teleporters"};
    Layer otherVolumes{this, "Other volumes", "Other volumes color", "Other", false, {0.20f, 0.80f, 0.80f, 1.f},
                       "Every other kind of volume: post process, reverb, fog, streaming..."};
    Layer cylinders{this, "Cylinders", "Cylinders color", "Cylinders", true, {0.95f, 0.85f, 0.25f, 1.f},
                    "Cylinder collision of pawns and other moving actors (looked up live)"};
    Layer spawns{this, "Spawn points", "Spawn points color", "Spawns", false, {0.95f, 0.95f, 0.95f, 1.f},
                 "Player starts: level starts, time trial and tutorial starts"};
    Layer pickups{this, "Pickups", "Pickups color", "Pickups", false, {0.95f, 0.45f, 0.75f, 1.f},
                  "Weapon pickups and stash points (where they are placed, not dropped weapons)"};
    Layer aiOnly{this, "AI volumes", "AI volumes color", "AI only", false, {0.55f, 0.60f, 0.35f, 1.f},
                 "Volumes and areas only AI uses: AI blocking volumes (not walls for the player), path limits, cover "
                 "groups, grenade target areas"};
    Layer navigation{this, "Navigation points", "Navigation points color", "Navigation", false,
                     {0.45f, 0.65f, 0.85f, 1.f},
                     "The AI's path network: path nodes and move hints (vault, wallrun, jump, slide). Many per level"};
    ctl::Custom filterBox{this, [this](float w) { DrawFilter(w); }};

    ctl::Section displaySection{this, "Display", true};
    ctl::Combo style{this, "Style", {"Solid", "Solid + outline", "Wireframe"}, 1};
    ctl::Slider opacity{this, "Opacity", 0.55f, 0.1f, 1.f, "%.2f", 0.05f, "1 = fully opaque"};
    ctl::Combo depth{this, "Depth", {"Hidden by the world", "Always on top"}, 0,
                     "Hidden by the world: drawn inside the game's frame, so walls and props hide the volumes behind "
                     "them. Always on top: drawn over the finished frame, nothing hides them"};
    ctl::Slider maxDistance{this, "Max distance", 6000.f, 500.f, 40000.f, "%.0f", 500.f,
                            "Volumes farther away than this are not drawn"};
    ctl::Checkbox fade{this, "Fade with distance", true, "Volumes fade out over the last quarter of the max distance "
                                                         "instead of popping out of view"};
    ctl::Checkbox labels{this, "Labels", false, "Actor names of the triggers, volumes and other objects shown"};

    ctl::Section inspectSection{this, "Inspect", true};
    ctl::Checkbox lookAt{this, "Look-at info", true,
                         "Under the crosshair: the name, kind, distance and size of the volume you are looking at"};
    ctl::Checkbox highlight{this, "Highlight looked-at", true, "Outline the volume you are looking at in white"};
    ctl::Checkbox insideList{this, "Inside list", true,
                             "Top left: the triggers, kill zones, checkpoints and volumes the player is inside right now"};
    ctl::Checkbox ownCylinder{this, "Own cylinder", false,
                              "Your own collision cylinder, as an outline (needs Cylinders on): for checking clearance"};
    ctl::Custom inspectInfo{this, [this](float w) { DrawInspectInfo(w); }};

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
