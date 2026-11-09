// Collision view: trigger, kill, checkpoint, blocking and movement volumes drawn as solid, translucent geometry, and the
// actors placed with them (player starts, pickups, AI-only volumes and areas, navigation points).
// The volumes are collected from the SDK on the game thread (game/debug_world.*) and drawn by the Direct3D 9 renderer in
// the EndScene / StretchRect hooks (render/debug_renderer.*). This module only owns the settings and wires the two together.
#include "core/module.h"
#include "game/debug_world.h"
#include "game/game.h"
#include "game/static_world.h"
#include "render/debug_renderer.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>
#include <array>
#include <cctype>
#include <initializer_list>
#include <cstdio>
#include <string>

namespace {

ImU32 ToImColor(std::uint32_t argb, float alpha = 1.f)
{
    const auto a = static_cast<int>(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f);
    return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, a);
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
        otherVolumes.VisibleIf([] { return false; });     // Drawn in the layer grid (DrawLayers)
        in(displaySection, {&style, &opacity, &depth, &maxDistance, &fade, &labels});
        labelDistance.VisibleIf([this] { return displaySection && labels.Get(); });
        in(inspectSection, {&lookAt, &highlight, &insideList, &ownCylinder, &inspectInfo});
        in(staticSection, {&staticOn, &staticGrid, &nonColliding, &staticStyle, &staticOpacity, &staticRange,
                           &staticBudget, &staticFilterBox, &staticStatus});
        staticOpacity.VisibleIf([this] { return staticSection && staticStyle.Get() != 0; });
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
        staticworld::Stop();
        staticShown_ = false;
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

        // Static geometry: scanned and built only while it is shown
        if (staticOn) {
            staticworld::Update(StaticConfig());
            staticShown_ = true;
        } else if (staticShown_) {
            staticworld::Stop();
            staticShown_ = false;
        }
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
        return {&triggers, &killZones, &checkpoints, &blocking, &movement, &miscVolumes, &cylinders,
                &spawns, &pickups, &aiOnly, &navigation, &postProcessVolumes, &audioVolumes, &fogVolumes,
                &streamingVolumes};
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
        c.labelDistance = labelDistance;
        c.useCache = useCache;
        c.scanCached = scanCached;

        const auto layers = Layers();
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const auto& rgba = layers[i]->color.Get();
            c.enabled[i] = layers[i]->show && (!debugworld::IsOtherVolume(static_cast<int>(i)) || otherVolumes);
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
        s.staticOpacity = staticStyle.Get() == 0 ? 0.f : staticOpacity.Get();
        s.aspect = aspectOverride;
        return s;
    }

    // Labels are drawn with ImGui (render thread) over the projected center of each volume: a small dark plate with the
    // layer's color on its edge, centered above a dot on the point. The nearest go first and the rest only where they
    // don't overlap one already placed; they shrink a little and fade toward the label range. The volume under the
    // crosshair always gets its label, outlined.
    void DrawLabels() const
    {
        const auto scene = debugrender::CurrentScene();
        if (!scene || scene->labels.empty()) return;
        const rmath::Camera cam = debugrender::CurrentCamera();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float range = std::max(labelDistance.Get(), 1.f);

        struct Item {
            const rmesh::Label* label;
            float x, y, distance;
            bool look;
        };
        std::vector<Item> items;
        items.reserve(scene->labels.size());
        for (const auto& label : scene->labels) {
            float x = 0.f, y = 0.f;
            if (!debugrender::WorldToScreen(label.pos, x, y)) continue;     // Behind the camera
            if (x < -60.f || y < -30.f || x > screen.x + 60.f || y > screen.y + 30.f) continue;
            const bool look = info_.look.valid && label.text == info_.look.name;
            items.push_back({&label, x, y, rmath::Length(label.pos - cam.pos), look});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.look != b.look ? a.look : a.distance < b.distance;
        });

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImFont* font = ImGui::GetFont();
        const float base = ImGui::GetFontSize();
        std::vector<ImVec4> placed;     // x0, y0, x1, y1
        constexpr int kMaxShown = 60;
        for (const Item& it : items) {
            if (static_cast<int>(placed.size()) >= kMaxShown) break;
            const float t = std::clamp(it.distance / range, 0.f, 1.f);
            const float size = it.look ? base : base * (1.f - 0.2f * t);
            const float alpha = it.look ? 1.f : (t < 0.6f ? 1.f : std::max(0.25f, 1.f - (t - 0.6f) / 0.4f * 0.75f));
            const char* text = it.label->text.c_str();
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
            constexpr float padX = 5.f, padY = 2.f, bar = 3.f, gap = 6.f;
            const float w = ts.x + 2.f * padX + bar, h = ts.y + 2.f * padY;
            const ImVec2 p0{std::round(it.x - w * 0.5f), std::round(it.y - gap - h)}, p1{p0.x + w, p0.y + h};

            bool overlaps = false;
            for (const ImVec4& r : placed)
                if (p0.x < r.z + 2.f && p1.x > r.x - 2.f && p0.y < r.w + 2.f && p1.y > r.y - 2.f) {
                    overlaps = true;
                    break;
                }
            if (overlaps && !it.look) continue;
            placed.push_back({p0.x, p0.y, p1.x, p1.y});

            const ImU32 color = ToImColor(it.label->color, alpha);
            dl->AddCircleFilled({it.x, it.y}, 2.5f, color);                                         // The point itself
            dl->AddLine({it.x, it.y - 2.5f}, {it.x, p1.y}, ToImColor(it.label->color, alpha * 0.6f)); // Up to the plate
            dl->AddRectFilled(p0, p1, IM_COL32(12, 12, 14, static_cast<int>(170.f * alpha)), 3.f);
            dl->AddRectFilled(p0, {p0.x + bar, p1.y}, color, 3.f, ImDrawFlags_RoundCornersLeft);
            if (it.look) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 230), 3.f);
            dl->AddText(font, size, {p0.x + bar + padX, p0.y + padY}, IM_COL32(240, 240, 245, static_cast<int>(255.f * alpha)),
                        text);
        }
    }

    // Every kind of volume in two columns: shown or not, and its color as a small swatch next to it.
    // One layer: its color, shown or not, and how many are drawn of how many this level has.
    void DrawLayerCell(Layer& layer, std::size_t i)
    {
        ImGui::PushID(layer.label);
        auto rgba = layer.color.Get();
        if (ImGui::ColorEdit3("##color", rgba.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            layer.color.Set(rgba);
        ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
        bool shown = layer.show;
        if (ImGui::Checkbox(layer.label, &shown)) layer.show.Set(shown);
        if (layer.tooltip[0]) ImGui::SetItemTooltip("%s", layer.tooltip);
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

    // Every kind of volume in two columns. The other volumes are one switch there, and their kinds below it.
    void DrawLayers(float)
    {
        const auto all = Layers();
        if (ImGui::BeginTable("##layers", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (std::size_t i = 0; i < all.size(); ++i) {     // In the order of debugworld::Category
                const int category = static_cast<int>(i);
                if (debugworld::IsOtherVolume(category) && category != static_cast<int>(debugworld::Category::Volume))
                    continue;
                ImGui::TableNextColumn();
                if (category != static_cast<int>(debugworld::Category::Volume)) {
                    DrawLayerCell(*const_cast<Layer*>(all[i]), i);
                    continue;
                }
                // The group switch, with the counts of all its kinds together
                bool on = otherVolumes;
                if (ImGui::Checkbox("Other volumes##group", &on)) otherVolumes.Set(on);
                ImGui::SetItemTooltip("Every other kind of volume, in the groups below: post process, audio, fog, "
                                      "streaming and the rest");
                std::size_t drawn = 0, remembered = 0;
                for (std::size_t j = 0; j < all.size(); ++j)
                    if (debugworld::IsOtherVolume(static_cast<int>(j))) {
                        drawn += info_.drawn[j];
                        remembered += info_.remembered[j];
                    }
                if (remembered > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%zu/%zu", drawn, remembered);
                }
            }
            ImGui::EndTable();
        }

        // The kinds of other volumes, indented under the group
        ImGui::Indent(ImGui::GetFrameHeight());
        ImGui::BeginDisabled(!otherVolumes);
        if (ImGui::BeginTable("##othervolumes", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (!debugworld::IsOtherVolume(static_cast<int>(i))) continue;
                ImGui::TableNextColumn();
                DrawLayerCell(*const_cast<Layer*>(all[i]), i);
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::Unindent(ImGui::GetFrameHeight());
    }

    // ---- Static geometry ---------------------------------------------------------------------------------------------
    // Every static layer, in the order of rstatic::Category
    std::array<const Layer*, rstatic::kCategoryCount> StaticLayers() const
    {
        return {&architecture, &movers, &props, &hiddenWalls, &otherMeshes, &boundsOnly};
    }

    staticworld::Config StaticConfig() const
    {
        staticworld::Config c;
        const auto layers = StaticLayers();
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const auto& rgba = layers[i]->color.Get();
            c.enabled[i] = layers[i]->show;
            c.colors[i] = rmesh::PackColor(rgba[0], rgba[1], rgba[2], 1.f);
        }
        c.nonColliding = nonColliding;
        c.fill = staticStyle.Get() != 0;
        c.outline = staticStyle.Get() != 2;
        c.maxDistance = staticRange;
        c.fade = fade;
        std::string f = staticFilter_;
        f.erase(0, f.find_first_not_of(' '));
        f.erase(f.find_last_not_of(' ') + 1);
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        c.filter = f;
        c.vertexBudget = static_cast<std::size_t>(staticBudget.Get()) * 1000;
        return c;
    }

    // Like the volume layers: shown or not, color, and drawn / found in this level.
    void DrawStaticLayers(float)
    {
        const auto all = StaticLayers();
        const auto info = staticworld::GetInfo();
        ImGui::BeginDisabled(!staticOn);
        if (ImGui::BeginTable("##static", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                Layer* layer = const_cast<Layer*>(all[i]);
                ImGui::TableNextColumn();
                ImGui::PushID(layer->label);
                auto rgba = layer->color.Get();
                if (ImGui::ColorEdit3("##color", rgba.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                    layer->color.Set(rgba);
                ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
                bool shown = layer->show;
                if (ImGui::Checkbox(layer->label, &shown)) layer->show.Set(shown);
                if (layer->tooltip[0]) ImGui::SetItemTooltip("%s", layer->tooltip);
                if (staticOn && info.found[i] > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%zu/%zu", info.drawn[i], info.found[i]);
                    ImGui::SetItemTooltip("%zu drawn (in range, matching the filters) of %zu in this level", info.drawn[i],
                                          info.found[i]);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
    }

    void DrawStaticFilter(float width)
    {
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##staticfilter", "Filter meshes by name", staticFilter_, sizeof staticFilter_);
        ImGui::SetItemTooltip("Only meshes whose actor or mesh name contains this (e.g. door, rail, glass)");
    }

    static void DrawStaticStatus(float width)
    {
        const auto info = staticworld::GetInfo();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if (info.scanning)
            ImGui::TextDisabled("Reading the level... %.0f%% (%zu meshes so far)", info.scanProgress * 100.f, info.meshes);
        else if (!info.map.empty())
            ImGui::TextDisabled("%zu meshes; drawing %zu k vertices, built in %.1f ms on a worker thread", info.meshes,
                                info.vertices / 1000, info.buildMs);
        if (!info.map.empty())
            ImGui::TextDisabled("Game thread: %.2f ms a tick on average (slowest %.1f ms)", info.tickAvgMs, info.tickMaxMs);
        if (info.budgetHit)
            ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Vertex limit reached: the farthest meshes are left out");
        ImGui::PopTextWrapPos();
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
    bool staticShown_ = false;          // Game thread: static geometry was being drawn
    char staticFilter_[64] = {};
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
    // The other volumes: one switch for the group (saved under its old name), and a layer for each kind
    ctl::Checkbox otherVolumes{this, "Other volumes", false};
    Layer postProcessVolumes{this, "Other: post process", "Other: post process color", "Post process", true,
                             {0.95f, 0.80f, 0.35f, 1.f}, "Post process, color scale, light and reflection volumes"};
    Layer audioVolumes{this, "Other: audio", "Other: audio color", "Audio", true, {0.55f, 0.85f, 0.45f, 1.f},
                       "Reverb and other audio volumes"};
    Layer fogVolumes{this, "Other: fog", "Other: fog color", "Fog", true, {0.72f, 0.72f, 0.82f, 1.f},
                     "Fog volumes and their density shapes"};
    Layer streamingVolumes{this, "Other: streaming", "Other: streaming color", "Streaming", true,
                           {0.35f, 0.55f, 0.95f, 1.f},
                           "Level streaming volumes (where parts of the level load), cull distance and portal volumes"};
    Layer miscVolumes{this, "Other: misc", "Other: misc color", "Misc", true, {0.20f, 0.80f, 0.80f, 1.f},
                      "Every other kind of volume (physics force field excludes, plain volumes, ...)"};
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
    ctl::Slider labelDistance{this, "Label range", 3000.f, 250.f, 20000.f, "%.0f", 250.f,
                              "Labels only for volumes this close; the nearest win where labels would overlap"};

    ctl::Section inspectSection{this, "Inspect", true};
    ctl::Checkbox lookAt{this, "Look-at info", true,
                         "Under the crosshair: the name, kind, distance and size of the volume you are looking at"};
    ctl::Checkbox highlight{this, "Highlight looked-at", true, "Outline the volume you are looking at in white"};
    ctl::Checkbox insideList{this, "Inside list", true,
                             "Top left: the triggers, kill zones, checkpoints and volumes the player is inside right now"};
    ctl::Checkbox ownCylinder{this, "Own cylinder", false,
                              "Your own collision cylinder, as an outline (needs Cylinders on): for checking clearance"};
    ctl::Custom inspectInfo{this, [this](float w) { DrawInspectInfo(w); }};

    ctl::Section staticSection{this, "Static geometry", false};
    ctl::Checkbox staticOn{this, "Show static geometry", false,
                           "The simple collision of the level's static meshes: walls, floors, rails, movers, props and "
                           "invisible walls. Read a little per frame and built on a worker thread"};
    ctl::Custom staticGrid{this, [this](float w) { DrawStaticLayers(w); }};
    Layer architecture{this, "Static: architecture", "Static: architecture color", "Architecture", true,
                       {0.78f, 0.78f, 0.82f, 1.f}, "Static mesh actors: the walls, floors, roofs and rails of the level"};
    Layer movers{this, "Static: movers", "Static: movers color", "Movers", true, {0.35f, 0.85f, 0.95f, 1.f},
                 "Doors, elevators and moving platforms (InterpActors), drawn where they are now"};
    Layer props{this, "Static: props", "Static: props color", "Props", true, {0.95f, 0.65f, 0.25f, 1.f},
                "Physics objects and other dynamic meshes (KActors), drawn where they are now"};
    Layer hiddenWalls{this, "Static: invisible", "Static: invisible color", "Invisible", true, {1.f, 0.30f, 0.85f, 1.f},
                      "Meshes the game doesn't draw but that still block you: invisible walls"};
    Layer otherMeshes{this, "Static: other", "Static: other color", "Other", false, {0.62f, 0.62f, 0.50f, 1.f},
                      "Static meshes of any other kind of actor"};
    Layer boundsOnly{this, "Static: bounds only", "Static: bounds only color", "Bounds", false, {0.50f, 0.50f, 0.62f, 1.f},
                     "Meshes without simple collision (they collide per polygon, which can't be read): their "
                     "bounding box, so it is only roughly where they block"};
    ctl::Checkbox nonColliding{this, "Include non-colliding", false,
                               "Also meshes you pass through (decoration). Off: only what blocks the player"};
    ctl::Combo staticStyle{this, "Static style", {"Outline", "Solid + outline", "Solid"}, 0,
                           "Outline keeps the view readable: filled walls everywhere hide the level"};
    ctl::Slider staticOpacity{this, "Static opacity", 0.25f, 0.05f, 1.f, "%.2f", 0.05f};
    ctl::Slider staticRange{this, "Static range", 3000.f, 500.f, 15000.f, "%.0f", 250.f,
                            "Meshes farther away than this are not drawn. Larger costs more to build and draw"};
    ctl::SliderInt staticBudget{this, "Vertex limit", 400, 50, 2000, "%d k", 50,
                                "The most vertices drawn; nearest meshes first. Lower it if the game slows down"};
    ctl::Custom staticFilterBox{this, [this](float w) { DrawStaticFilter(w); }};
    ctl::Custom staticStatus{this, [](float w) { DrawStaticStatus(w); }};

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
