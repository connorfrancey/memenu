#include "features/tas/timeline_view.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tasui {

using tas::timeline::Hold;

namespace {

constexpr float kLabelW = 110.f;      // the names column
constexpr float kRulerH = 22.f;
constexpr float kCameraH = 26.f;
constexpr float kRowH = 22.f;
constexpr float kEdge = 5.f;          // pixels at a bar's ends that resize it
constexpr float kKeyHit = 7.f;        // pixels around a camera key that pick it

ImU32 TrackColor(const std::string& name, float alpha = 1.f) { return KeyColor(name, alpha); }

} // namespace

// A steady color per key name, so a key keeps its color when tracks come and go.
ImU32 KeyColor(const std::string& name, float alpha)
{
    std::uint32_t h = 2166136261u;
    for (char c : name) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    const float hue = static_cast<float>(h % 360u) / 360.f;
    ImVec4 c;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, c.x, c.y, c.z);
    c.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

namespace {

// Ruler step: a round number of frames at least `minPx` apart.
int RulerStep(float pxPerFrame, float minPx)
{
    static const int steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 3000, 6000, 18000};
    for (int s : steps)
        if (s * pxPerFrame >= minPx) return s;
    return steps[std::size(steps) - 1];
}

std::string FrameTime(std::size_t frame, float hz)
{
    const double secs = static_cast<double>(frame) / (hz > 0.f ? hz : 60.f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%05.2f", static_cast<int>(secs / 60.0), std::fmod(secs, 60.0));
    return buf;
}

} // namespace

void TimelineView::Draw(bool& open, const tas::Status& st)
{
    ImGui::SetNextWindowSize({900.f, 340.f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Timeline##tas", &open)) {
        ImGui::End();
        return;
    }
    const bool readOnly = st.mode == tas::Mode::Recording;
    tas::WithReplay([&](tas::Replay& r) {
        frames_ = r.frames.size();
        DrawToolbar(r, st, readOnly);
        DrawCanvas(r, st, readOnly);
        DrawDetails(r, readOnly);
    });
    ImGui::End();
}

void TimelineView::DrawToolbar(tas::Replay& r, const tas::Status& st, bool readOnly)
{
    ImGui::SetNextItemWidth(130.f);
    ImGui::SliderFloat("##zoom", &pxPerFrame_, 0.5f, 24.f, "zoom %.1f px", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Pixels per frame (ctrl + wheel over the timeline)");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow_);
    ImGui::SetItemTooltip("Keep the frame being played or recorded in view");
    ImGui::SameLine();
    const std::size_t at = st.mode == tas::Mode::Idle ? 0 : st.frame;
    ImGui::TextDisabled("%zu frames, %s | at %zu", r.frames.size(), FrameTime(r.frames.size(), r.fixedHz).c_str(), at);

    ImGui::SameLine();
    ImGui::BeginDisabled(readOnly);
    ImGui::SetNextItemWidth(120.f);
    const bool enter = ImGui::InputTextWithHint("##track", "key, e.g. W", newTrack_, sizeof newTrack_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add track") || enter) && newTrack_[0]) {
        if (std::find(extraTracks_.begin(), extraTracks_.end(), newTrack_) == extraTracks_.end())
            extraTracks_.emplace_back(newTrack_);
        r.NameIndex(newTrack_);
        newTrack_[0] = 0;
    }
    ImGui::SetItemTooltip("A row for a key the macro doesn't use yet. Unreal key names: W, SpaceBar, LeftShift, "
                          "LeftMouseButton...");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::InputInt("##len", &newHold_, 0, 0);
    newHold_ = std::clamp(newHold_, 1, 100000);
    ImGui::SetItemTooltip("Frames a double click holds a key for");
    ImGui::EndDisabled();
    if (readOnly) {
        ImGui::SameLine();
        ImGui::TextDisabled("(read only while recording)");
    }
}

Hold TimelineView::Preview() const
{
    Hold h = drag_.orig;
    switch (drag_.what) {
        case Drag::What::Move: {
            const int start = std::max(0, static_cast<int>(h.start) + drag_.delta);
            h.end = static_cast<std::uint32_t>(start) + h.Length();
            h.start = static_cast<std::uint32_t>(start);
            break;
        }
        case Drag::What::Start:
            h.start = static_cast<std::uint32_t>(std::clamp(static_cast<int>(h.start) + drag_.delta, 0,
                                                            static_cast<int>(h.end) - 1));
            break;
        case Drag::What::End:
            h.end = static_cast<std::uint32_t>(std::max(static_cast<int>(h.start) + 1,
                                                        static_cast<int>(h.end) + drag_.delta));
            break;
        default: break;
    }
    return h;
}

void TimelineView::Commit(tas::Replay& r)
{
    switch (drag_.what) {
        case Drag::What::Move:
        case Drag::What::Start:
        case Drag::What::End: {
            if (drag_.delta == 0) break;
            const Hold h = Preview();
            auto holds = drag_.others;
            holds.push_back(h);
            tas::timeline::SetHolds(r, drag_.name, holds);
            // Select what it became (merged with a neighbour, it is the hold that contains it)
            sel_ = {Selection::Kind::Hold, drag_.name, h, 0};
            for (const Hold& now : tas::timeline::Holds(r, drag_.name))
                if (now.start <= h.start && now.end >= h.end) sel_.hold = now;
            break;
        }
        case Drag::What::Camera: {
            if (drag_.delta == 0 || r.frames.empty()) break;
            const auto to = static_cast<std::uint32_t>(std::clamp(static_cast<int>(drag_.camFrame) + drag_.delta, 0,
                                                                  static_cast<int>(r.frames.size()) - 1));
            if (tas::timeline::MoveCameraKey(r, drag_.camFrame, to)) sel_ = {Selection::Kind::Camera, 0, {}, to};
            break;
        }
        default: break;
    }
    drag_ = {};
}

void TimelineView::DrawCanvas(tas::Replay& r, const tas::Status& st, bool readOnly)
{
    using tas::timeline::Track;
    std::vector<Track> tracks = tas::timeline::Tracks(r);

    // Rows added by name that have no inputs yet (dropped once the replay no longer has the name: another was loaded)
    extraTracks_.erase(std::remove_if(extraTracks_.begin(), extraTracks_.end(), [&](const std::string& n) {
        return std::find(r.names.begin(), r.names.end(), n) == r.names.end();
    }), extraTracks_.end());
    for (const std::string& n : extraTracks_) {
        const auto idx = static_cast<std::uint16_t>(std::find(r.names.begin(), r.names.end(), n) - r.names.begin());
        if (std::none_of(tracks.begin(), tracks.end(), [&](const Track& t) { return t.name == idx; })) {
            Track t;
            t.name = idx;
            const auto firstAxis = std::find_if(tracks.begin(), tracks.end(), [](const Track& x) { return x.axis; });
            tracks.insert(firstAxis, std::move(t));
        }
    }

    const auto frameCount = static_cast<std::uint32_t>(r.frames.size());
    const float detailsH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4.f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##canvas", {avail.x, std::max(90.f, avail.y - detailsH)}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float viewW = ImGui::GetWindowWidth(), viewH = ImGui::GetWindowHeight();

    // Ctrl + wheel: zoom with the frame under the mouse staying put
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.f) {
        const float mouseOff = io.MousePos.x - winPos.x - kLabelW;
        const float anchor = (ImGui::GetScrollX() + mouseOff) / pxPerFrame_;
        pxPerFrame_ = std::clamp(pxPerFrame_ * std::pow(1.2f, io.MouseWheel), 0.5f, 24.f);
        ImGui::SetScrollX(std::max(0.f, anchor * pxPerFrame_ - mouseOff));
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float ppf = pxPerFrame_;
    const float totalW = kLabelW + (static_cast<float>(frameCount) + 60.f) * ppf;
    const float totalH = kRulerH + kCameraH + static_cast<float>(tracks.size()) * kRowH + 4.f;
    const auto X = [&](double frame) { return origin.x + kLabelW + static_cast<float>(frame) * ppf; };
    const auto FrameAt = [&](float x) { return std::max(0, static_cast<int>(std::floor((x - origin.x - kLabelW) / ppf))); };
    const float cameraY = origin.y + kRulerH;
    const float rowsY = cameraY + kCameraH;

    ImGui::InvisibleButton("##hit", {std::max(totalW, viewW - 4.f), std::max(totalH, 10.f)},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 m = io.MousePos;
    const int mouseFrame = FrameAt(m.x);
    const bool inLabels = m.x < winPos.x + kLabelW;

    // ---- what is under the mouse ----------------------------------------------------------------------------------------------
    struct Hit {
        enum class Kind { None, Ruler, Camera, CameraKey, Track, Hold } kind = Kind::None;
        int track = -1;
        Hold hold;
        Drag::What edge = Drag::What::Move;
        std::uint32_t camFrame = 0;
    } hit;
    if (hovered && !inLabels) {
        if (m.y < cameraY) {
            hit.kind = Hit::Kind::Ruler;
        } else if (m.y < rowsY) {
            hit.kind = Hit::Kind::Camera;
            for (const tas::CameraKey& k : r.camera)
                if (std::abs(X(k.frame + 0.5) - m.x) <= kKeyHit) { hit.kind = Hit::Kind::CameraKey; hit.camFrame = k.frame; }
        } else {
            const int row = static_cast<int>((m.y - rowsY) / kRowH);
            if (row >= 0 && row < static_cast<int>(tracks.size())) {
                hit.kind = Hit::Kind::Track;
                hit.track = row;
                for (const Hold& h : tracks[row].holds) {
                    const float x0 = X(h.start), x1 = X(h.end);
                    if (m.x < x0 - 1.f || m.x > x1 + 1.f) continue;
                    hit.kind = Hit::Kind::Hold;
                    hit.hold = h;
                    const float edge = std::min(kEdge, (x1 - x0) * 0.3f);
                    hit.edge = m.x - x0 <= edge ? Drag::What::Start : x1 - m.x <= edge ? Drag::What::End : Drag::What::Move;
                }
            }
        }
    }
    if (hit.kind == Hit::Kind::Hold && hit.edge != Drag::What::Move && !readOnly)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (drag_.what == Drag::What::Start || drag_.what == Drag::What::End) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // ---- mouse ------------------------------------------------------------------------------------------------------------------
    const bool editable = !readOnly;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && editable) {
        if (hit.kind == Hit::Kind::Camera && mouseFrame < static_cast<int>(frameCount)) {
            // A new key where the camera already is on that frame (or the nearest key's direction)
            tas::CameraKey k;
            k.frame = static_cast<std::uint32_t>(mouseFrame);
            if (!tas::timeline::CameraAt(r, k.frame, k.yaw, k.pitch) && !r.camera.empty()) {
                const tas::CameraKey& near = k.frame < r.camera.front().frame ? r.camera.front() : r.camera.back();
                k.yaw = near.yaw;
                k.pitch = near.pitch;
            }
            tas::timeline::SetCameraKey(r, k);
            sel_ = {Selection::Kind::Camera, 0, {}, k.frame};
        } else if (hit.kind == Hit::Kind::Track && !tracks[hit.track].axis) {
            const Track& t = tracks[hit.track];
            auto holds = t.holds;
            const Hold h{static_cast<std::uint32_t>(mouseFrame), static_cast<std::uint32_t>(mouseFrame + newHold_)};
            holds.push_back(h);
            tas::timeline::SetHolds(r, t.name, holds);
            sel_ = {Selection::Kind::Hold, t.name, h, 0};
        }
        drag_ = {};
    } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_ = {};
        drag_.startX = m.x;
        switch (hit.kind) {
            case Hit::Kind::Ruler:
                if (editable) drag_.what = Drag::What::Seek;
                break;
            case Hit::Kind::CameraKey:
                sel_ = {Selection::Kind::Camera, 0, {}, hit.camFrame};
                if (editable) { drag_.what = Drag::What::Camera; drag_.camFrame = hit.camFrame; }
                break;
            case Hit::Kind::Hold: {
                const Track& t = tracks[hit.track];
                sel_ = {Selection::Kind::Hold, t.name, hit.hold, 0};
                if (!editable) break;
                drag_.what = hit.edge;
                drag_.name = t.name;
                drag_.orig = hit.hold;
                for (const Hold& h : t.holds)
                    if (!(h == hit.hold)) drag_.others.push_back(h);
                break;
            }
            default:
                sel_ = {};
                break;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && editable) {
        if (hit.kind == Hit::Kind::Hold) {
            const Track& t = tracks[hit.track];
            std::vector<Hold> holds;
            for (const Hold& h : t.holds)
                if (!(h == hit.hold)) holds.push_back(h);
            tas::timeline::SetHolds(r, t.name, holds);
            if (sel_.kind == Selection::Kind::Hold && sel_.name == t.name && sel_.hold == hit.hold) sel_ = {};
        } else if (hit.kind == Hit::Kind::CameraKey) {
            tas::timeline::EraseCameraKey(r, hit.camFrame);
            if (sel_.kind == Selection::Kind::Camera && sel_.frame == hit.camFrame) sel_ = {};
        }
    }
    if (drag_.what != Drag::What::None && ImGui::IsItemActive())
        drag_.delta = static_cast<int>(std::lround((m.x - drag_.startX) / ppf));
    if (drag_.what != Drag::What::None && ImGui::IsItemDeactivated()) {
        if (drag_.what == Drag::What::Seek) {
            tas::Goto(static_cast<std::size_t>(std::min(mouseFrame, static_cast<int>(frameCount))));
            drag_ = {};
        } else {
            Commit(r);
        }
    }

    // Follow the session
    const bool running = st.mode != tas::Mode::Idle && !st.armed;
    if (follow_ && running && drag_.what == Drag::What::None) {
        const float px = X(static_cast<double>(st.frame));
        if (px < winPos.x + kLabelW + 20.f || px > winPos.x + viewW - 40.f)
            ImGui::SetScrollX(std::max(0.f, static_cast<float>(st.frame) * ppf - (viewW - kLabelW) * 0.3f));
    }

    // ---- drawing --------------------------------------------------------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 lineCol = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
    const float left = winPos.x + kLabelW, right = winPos.x + viewW;
    const int f0 = std::max(0, FrameAt(left) - 1);
    const int f1 = FrameAt(right) + 1;

    dl->PushClipRect({left, winPos.y}, {right, winPos.y + viewH}, true);

    // Row backgrounds, then the part past the end shaded
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const float y = rowsY + static_cast<float>(i) * kRowH;
        if (i % 2) dl->AddRectFilled({left, y}, {right, y + kRowH}, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
    }
    dl->AddRectFilled({X(frameCount), origin.y}, {right, origin.y + totalH}, ImGui::GetColorU32(ImGuiCol_FrameBg));

    // Ruler
    const int step = RulerStep(ppf, 64.f);
    const int minor = std::max(1, RulerStep(ppf, 8.f));
    for (int f = f0 - f0 % minor; f <= f1; f += minor) {
        const float x = X(f);
        const bool major = f % step == 0;
        dl->AddLine({x, origin.y + (major ? 4.f : 14.f)}, {x, origin.y + kRulerH}, major ? dimCol : lineCol);
        if (major) {
            char label[16];
            std::snprintf(label, sizeof label, "%d", f);
            dl->AddText({x + 3.f, origin.y + 2.f}, dimCol, label);
            dl->AddLine({x, cameraY}, {x, origin.y + totalH}, ImGui::GetColorU32(ImGuiCol_Border, 0.35f));
        }
    }
    dl->AddLine({left, cameraY}, {right, cameraY}, lineCol);
    dl->AddLine({left, rowsY}, {right, rowsY}, lineCol);

    // Camera: the range the keys drive, and the keys as diamonds
    if (!r.camera.empty()) {
        const float y = cameraY + kCameraH * 0.5f;
        dl->AddRectFilled({X(r.camera.front().frame + 0.5), y - 2.f}, {X(r.camera.back().frame + 0.5), y + 2.f},
                          ImGui::GetColorU32(ImGuiCol_PlotLines, 0.5f), 2.f);
        for (const tas::CameraKey& k : r.camera) {
            std::uint32_t frame = k.frame;
            if (drag_.what == Drag::What::Camera && drag_.camFrame == k.frame)
                frame = static_cast<std::uint32_t>(std::clamp(static_cast<int>(k.frame) + drag_.delta, 0,
                                                              static_cast<int>(frameCount) - 1));
            const float x = X(frame + 0.5);
            const bool selected = sel_.kind == Selection::Kind::Camera && sel_.frame == k.frame;
            const float s = selected ? 7.f : 6.f;
            const ImVec2 pts[4] = {{x, y - s}, {x + s, y}, {x, y + s}, {x - s, y}};
            const ImU32 fill = k.ease == tas::Ease::Hold ? ImGui::GetColorU32(ImGuiCol_PlotHistogram)
                                                         : ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
            dl->AddConvexPolyFilled(pts, 4, fill);
            if (selected) dl->AddPolyline(pts, 4, textCol, ImDrawFlags_Closed, 1.5f);
        }
    }

    // Tracks
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const Track& t = tracks[i];
        const float y0 = rowsY + static_cast<float>(i) * kRowH + 3.f, y1 = y0 + kRowH - 6.f;
        const std::string& name = r.Name(t.name);
        if (t.axis) {
            const ImU32 col = TrackColor(name, 0.8f);
            auto it = std::lower_bound(t.moves.begin(), t.moves.end(), static_cast<std::uint32_t>(f0));
            for (; it != t.moves.end() && static_cast<int>(*it) <= f1; ++it) {
                const float x = X(*it);
                dl->AddRectFilled({x, y0 + 3.f}, {x + std::max(1.f, ppf - 1.f), y1 - 3.f}, col);
            }
            continue;
        }
        const bool dragging = drag_.what >= Drag::What::Move && drag_.name == t.name;
        const auto drawHold = [&](const Hold& h, bool selected) {
            const float x0 = X(h.start), x1 = std::max(X(h.end) - 1.f, x0 + 2.f);
            if (x1 < left || x0 > right) return;
            dl->AddRectFilled({x0, y0}, {x1, y1}, TrackColor(name, selected ? 1.f : 0.8f), 3.f);
            if (selected) dl->AddRect({x0, y0}, {x1, y1}, textCol, 3.f, 0, 1.5f);
            if (x1 - x0 > 30.f) {
                char len[16];
                std::snprintf(len, sizeof len, "%u", h.Length());
                dl->AddText({x0 + 4.f, y0 + (y1 - y0 - ImGui::GetFontSize()) * 0.5f}, IM_COL32(20, 20, 20, 230), len);
            }
        };
        if (dragging) {
            for (const Hold& h : drag_.others) drawHold(h, false);
            drawHold(Preview(), true);
        } else {
            for (const Hold& h : t.holds)
                drawHold(h, sel_.kind == Selection::Kind::Hold && sel_.name == t.name && sel_.hold == h);
        }
    }

    // The end, the hovered frame and the session's frame
    dl->AddLine({X(frameCount), origin.y}, {X(frameCount), origin.y + totalH}, dimCol, 1.5f);
    if (hovered && !inLabels) dl->AddLine({X(mouseFrame), origin.y}, {X(mouseFrame), origin.y + totalH}, lineCol);
    if (running) {
        const float x = X(static_cast<double>(st.frame));
        dl->AddLine({x, origin.y}, {x, origin.y + totalH}, accent, 2.f);
        dl->AddTriangleFilled({x - 5.f, origin.y}, {x + 5.f, origin.y}, {x, origin.y + 7.f}, accent);
    }
    if (drag_.what == Drag::What::Seek) {
        const float x = X(mouseFrame);
        dl->AddLine({x, origin.y}, {x, origin.y + totalH}, accent, 1.f);
    }
    dl->PopClipRect();

    // Names column (stays put while the timeline scrolls)
    dl->AddRectFilled(winPos, {left, winPos.y + viewH}, ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK);
    dl->AddRectFilled(winPos, {left, winPos.y + viewH}, ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddLine({left, winPos.y}, {left, winPos.y + viewH}, lineCol);
    const float pad = style.FramePadding.x;
    const auto label = [&](float y, float h, ImU32 col, const char* text) {
        dl->AddText({winPos.x + pad, y + (h - ImGui::GetFontSize()) * 0.5f}, col, text);
    };
    label(origin.y, kRulerH, dimCol, hovered && !inLabels ? ("frame " + std::to_string(mouseFrame)).c_str() : "frame");
    label(cameraY, kCameraH, textCol, "Camera");
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const float y = rowsY + static_cast<float>(i) * kRowH;
        const std::string& name = r.Name(tracks[i].name);
        dl->AddRectFilled({winPos.x + 2.f, y + 6.f}, {winPos.x + 5.f, y + kRowH - 6.f}, TrackColor(name));
        label(y, kRowH, tracks[i].axis ? dimCol : textCol, name.c_str());
    }
    if (tracks.empty())
        dl->AddText({left + 8.f, rowsY + 4.f}, dimCol,
                    frameCount ? "No inputs. Add a track above, then double click its row"
                               : "No macro: record one, load one, or add a track and double click its row");

    if (hovered && hit.kind != Hit::Kind::None && drag_.what == Drag::What::None) {
        if (hit.kind == Hit::Kind::Hold)
            ImGui::SetTooltip("%s: frames %u-%u (%u)\nDrag to move, drag an end to resize, right click to delete",
                              r.Name(tracks[hit.track].name).c_str(), hit.hold.start, hit.hold.end - 1, hit.hold.Length());
        else if (hit.kind == Hit::Kind::CameraKey)
            ImGui::SetTooltip("Camera key on frame %u\nDrag to move, right click to delete", hit.camFrame);
        else if (hit.kind == Hit::Kind::Camera && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Double click: a camera key on frame %d", mouseFrame);
        else if (hit.kind == Hit::Kind::Ruler && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Click: go to frame %d (%s)", mouseFrame, FrameTime(mouseFrame, r.fixedHz).c_str());
    }

    ImGui::EndChild();
}

void TimelineView::DrawDetails(tas::Replay& r, bool readOnly)
{
    ImGui::BeginDisabled(readOnly);
    if (sel_.kind == Selection::Kind::Hold) {
        const auto holds = tas::timeline::Holds(r, sel_.name);
        if (std::find(holds.begin(), holds.end(), sel_.hold) == holds.end()) sel_ = {};
    }
    if (sel_.kind == Selection::Kind::Camera && !tas::timeline::FindCameraKey(r, sel_.frame)) sel_ = {};

    if (sel_.kind == Selection::Kind::Hold) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(r.Name(sel_.name).c_str());
        ImGui::SameLine();
        int start = static_cast<int>(sel_.hold.start), length = static_cast<int>(sel_.hold.Length());
        // Applied on Enter or the +/- buttons, not on every typed digit (a half typed number would merge holds)
        ImGui::SetNextItemWidth(110.f);
        bool apply = ImGui::InputInt("from frame", &start, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.f);
        apply |= ImGui::InputInt("frames held", &length, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
        if (apply) {
            std::vector<Hold> holds;
            for (const Hold& h : tas::timeline::Holds(r, sel_.name))
                if (!(h == sel_.hold)) holds.push_back(h);
            const Hold h{static_cast<std::uint32_t>(std::max(0, start)),
                         static_cast<std::uint32_t>(std::max(0, start) + std::max(1, length))};
            holds.push_back(h);
            tas::timeline::SetHolds(r, sel_.name, holds);
            sel_.hold = h;
            for (const Hold& now : tas::timeline::Holds(r, sel_.name))
                if (now.start <= h.start && now.end >= h.end) sel_.hold = now;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            std::vector<Hold> holds;
            for (const Hold& h : tas::timeline::Holds(r, sel_.name))
                if (!(h == sel_.hold)) holds.push_back(h);
            tas::timeline::SetHolds(r, sel_.name, holds);
            sel_ = {};
        }
    } else if (sel_.kind == Selection::Kind::Camera) {
        tas::CameraKey k = *tas::timeline::FindCameraKey(r, sel_.frame);
        bool changed = false;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Camera key, frame %u", k.frame);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        changed |= ImGui::DragFloat("yaw", &k.yaw, 0.5f, 0.f, 0.f, "%.1f deg");
        ImGui::SetItemTooltip("Not wrapped: 370 is a full turn further than 10, so the camera turns all the way round");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        changed |= ImGui::DragFloat("pitch", &k.pitch, 0.5f, -89.f, 89.f, "%.1f deg");
        ImGui::SameLine();
        static const char* eases[] = {"Linear", "Smooth", "Hold"};
        int ease = static_cast<int>(k.ease);
        ImGui::SetNextItemWidth(90.f);
        if (ImGui::Combo("to next", &ease, eases, 3)) {
            k.ease = static_cast<tas::Ease>(ease);
            changed = true;
        }
        if (changed) tas::timeline::SetCameraKey(r, k);
        ImGui::SameLine();
        if (ImGui::Button("From camera")) tas::KeyCameraFromView(k.frame);
        ImGui::SetItemTooltip("Take the look direction the player has right now (go to this frame first)");
        ImGui::SameLine();
        if (ImGui::Button("Go to")) tas::Goto(k.frame);
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            tas::timeline::EraseCameraKey(r, k.frame);
            sel_ = {};
        }
    } else {
        ImGui::TextDisabled("Double click a row to add a hold or a camera key. Drag bars and keys, drag bar ends to "
                            "resize, right click to delete. Click the ruler to go to a frame.");
    }
    ImGui::EndDisabled();
}

} // namespace tasui
