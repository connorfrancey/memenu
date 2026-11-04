#include "features/tas/timeline_view.h"

#include <imgui.h>

#include <algorithm>
#include <climits>
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
constexpr float kKeyHit = 7.f;        // pixels around a camera key / bookmark that pick it
constexpr float kMinZoom = 0.05f, kMaxZoom = 24.f;

ImU32 TrackColor(const std::string& name, float alpha = 1.f) { return KeyColor(name, alpha); }

// Ruler step: a round number of frames at least `minPx` apart.
int RulerStep(float pxPerFrame, float minPx)
{
    static const int steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 3000, 6000, 18000, 36000, 72000};
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

float NominalDt(const tas::Replay& r, std::size_t frame)
{
    if (r.fixedHz > 0.f) return 1.f / r.fixedHz;
    if (r.frames.empty()) return 1.f / 60.f;
    return r.frames[std::min(frame, r.frames.size() - 1)].dt;
}

std::string Count(std::size_t n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

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

tas::EditHistory& History()
{
    static tas::EditHistory history;
    return history;
}

// ---- the window ------------------------------------------------------------------------------------------------------------
void TimelineView::Draw(bool& open, const tas::Status& st)
{
    ImGui::SetNextWindowSize({900.f, 360.f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Timeline##tas", &open)) {
        ImGui::End();
        return;
    }
    const bool readOnly = st.mode == tas::Mode::Recording;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    tas::WithReplay([&](tas::Replay& r) {
        History().Watch(r);
        Prune(r);
        Shortcuts(r, st, readOnly, focused);
        DrawToolbar(r, st, readOnly);
        DrawCanvas(r, st, readOnly);
        DrawDetails(r, readOnly);
    });
    ImGui::End();
}

void TimelineView::Shortcuts(tas::Replay& r, const tas::Status& st, bool readOnly, bool focused)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (!focused || io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    const auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    if (ctrl && pressed(ImGuiKey_A)) SelectAll(r);
    if (pressed(ImGuiKey_Escape)) { holds_.clear(); cams_.clear(); }
    if (!ctrl && pressed(ImGuiKey_F)) fitNext_ = true;
    if (!ctrl && pressed(ImGuiKey_Space) && st.mode != tas::Mode::Idle) tas::TogglePause();
    if (ctrl && pressed(ImGuiKey_C)) CopySelection(r);
    if (readOnly) return;

    if (ctrl && !shift && pressed(ImGuiKey_Z)) { History().Undo(r); Prune(r); }
    if (ctrl && (pressed(ImGuiKey_Y) || (shift && pressed(ImGuiKey_Z)))) { History().Redo(r); Prune(r); }
    if (ctrl && pressed(ImGuiKey_X)) { CopySelection(r); DeleteSelection(r); }
    if (ctrl && pressed(ImGuiKey_V)) PasteAt(r, PasteFrame(st));
    if (ctrl && pressed(ImGuiKey_D)) Duplicate(r);
    if (pressed(ImGuiKey_Delete) || pressed(ImGuiKey_Backspace)) DeleteSelection(r);
    if (!ctrl && pressed(ImGuiKey_LeftArrow)) MoveSelection(r, shift ? -10 : -1);
    if (!ctrl && pressed(ImGuiKey_RightArrow)) MoveSelection(r, shift ? 10 : 1);
}

void TimelineView::DrawToolbar(tas::Replay& r, const tas::Status& st, bool readOnly)
{
    auto& history = History();
    ImGui::BeginDisabled(readOnly || !history.CanUndo());
    if (ImGui::Button("Undo")) { history.Undo(r); Prune(r); }
    ImGui::EndDisabled();
    if (history.CanUndo()) ImGui::SetItemTooltip("Undo %s (ctrl+Z)", history.UndoName().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(readOnly || !history.CanRedo());
    if (ImGui::Button("Redo")) { history.Redo(r); Prune(r); }
    ImGui::EndDisabled();
    if (history.CanRedo()) ImGui::SetItemTooltip("Redo %s (ctrl+Y)", history.RedoName().c_str());

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    ImGui::SliderFloat("##zoom", &pxPerFrame_, kMinZoom, kMaxZoom, "zoom %.2f px", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Pixels per frame (ctrl + wheel over the timeline)");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fitNext_ = true;
    ImGui::SetItemTooltip("The whole macro in the window (F)");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow_);
    ImGui::SetItemTooltip("Keep the frame being played or recorded in view");
    ImGui::SameLine();
    const std::size_t at = st.mode == tas::Mode::Idle ? 0 : st.frame;
    ImGui::TextDisabled("%zu frames, %s | at %zu", r.frames.size(), FrameTime(r.frames.size(), r.fixedHz).c_str(), at);

    ImGui::SameLine();
    ImGui::BeginDisabled(readOnly);
    ImGui::SetNextItemWidth(110.f);
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
    ImGui::SetNextItemWidth(60.f);
    ImGui::InputInt("##len", &newHold_, 0, 0);
    newHold_ = std::clamp(newHold_, 1, 100000);
    ImGui::SetItemTooltip("Frames a double click holds a key for");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip(
        "Click: select. Ctrl + click: add to the selection. Shift + drag: select a box.\n"
        "Drag: move the selection. Drag a bar's end: resize it. Double click: add a hold / camera key / bookmark.\n"
        "Right click: delete, split, duplicate, ease, insert or delete frames...\n\n"
        "Ctrl+Z / Ctrl+Y  undo / redo       Ctrl+C / X / V  copy / cut / paste (at the mouse)\n"
        "Ctrl+D  duplicate    Delete  delete    Left / Right  nudge a frame (shift: 10)\n"
        "Ctrl+A  select all   Esc  select nothing   F  fit   Space  pause / resume");
    if (readOnly) {
        ImGui::SameLine();
        ImGui::TextDisabled("(read only while recording)");
    }
}

// ---- the selection ---------------------------------------------------------------------------------------------------------
bool TimelineView::Selected(const KeyHold& h) const { return std::find(holds_.begin(), holds_.end(), h) != holds_.end(); }

bool TimelineView::SelectedCamera(std::uint32_t frame) const
{
    return std::find(cams_.begin(), cams_.end(), frame) != cams_.end();
}

void TimelineView::Select(const KeyHold& h, bool toggle)
{
    if (!toggle) { holds_ = {h}; cams_.clear(); return; }
    const auto it = std::find(holds_.begin(), holds_.end(), h);
    if (it != holds_.end()) holds_.erase(it);
    else holds_.push_back(h);
}

void TimelineView::SelectCamera(std::uint32_t frame, bool toggle)
{
    if (!toggle) { cams_ = {frame}; holds_.clear(); return; }
    const auto it = std::find(cams_.begin(), cams_.end(), frame);
    if (it != cams_.end()) cams_.erase(it);
    else cams_.push_back(frame);
}

void TimelineView::SelectAll(const tas::Replay& r)
{
    holds_.clear();
    cams_.clear();
    for (const auto& t : tas::timeline::Tracks(r))
        for (const Hold& h : t.holds) holds_.push_back({t.name, h});
    for (const auto& k : r.camera) cams_.push_back(k.frame);
}

void TimelineView::Prune(const tas::Replay& r)
{
    if (!holds_.empty()) {
        std::vector<KeyHold> keep;
        std::uint16_t lastName = UINT16_MAX;
        std::vector<Hold> holds;
        for (const KeyHold& k : holds_) {
            if (k.name != lastName) { holds = tas::timeline::Holds(r, k.name); lastName = k.name; }
            if (std::find(holds.begin(), holds.end(), k.hold) != holds.end()) keep.push_back(k);
        }
        holds_ = std::move(keep);
    }
    cams_.erase(std::remove_if(cams_.begin(), cams_.end(),
                               [&](std::uint32_t f) { return !tas::timeline::FindCameraKey(r, f); }),
                cams_.end());
}

int TimelineView::GroupDelta(const tas::Replay& r, int delta) const
{
    int lo = INT_MIN, hi = INT_MAX;
    for (const KeyHold& k : holds_) lo = std::max(lo, -static_cast<int>(k.hold.start));
    for (std::uint32_t f : cams_) {
        lo = std::max(lo, -static_cast<int>(f));
        hi = std::min(hi, static_cast<int>(r.frames.size()) - 1 - static_cast<int>(f));
    }
    return std::clamp(delta, lo, std::max(lo, hi));
}

// ---- edits -----------------------------------------------------------------------------------------------------------------
void TimelineView::MoveSelection(tas::Replay& r, int delta)
{
    delta = GroupDelta(r, delta);
    if (Empty() || delta == 0) return;
    History().Push(r, "Move " + Count(holds_.size() + cams_.size(), "item", "items"));
    holds_ = tas::timeline::MoveHolds(r, holds_, delta);
    cams_ = tas::timeline::MoveCameraKeys(r, cams_, delta);
}

void TimelineView::DeleteSelection(tas::Replay& r)
{
    if (Empty()) return;
    History().Push(r, "Delete " + Count(holds_.size() + cams_.size(), "item", "items"));
    tas::timeline::EraseHolds(r, holds_);
    for (std::uint32_t f : cams_) tas::timeline::EraseCameraKey(r, f);
    holds_.clear();
    cams_.clear();
}

void TimelineView::CopySelection(const tas::Replay& r)
{
    if (!Empty()) clip_ = tas::timeline::Copy(r, holds_, cams_);
}

void TimelineView::PasteAt(tas::Replay& r, std::uint32_t frame)
{
    if (clip_.Empty()) return;
    History().Push(r, "Paste " + Count(clip_.holds.size() + clip_.camera.size(), "item", "items"));
    cams_.clear();
    holds_ = tas::timeline::Paste(r, clip_, frame, &cams_);
}

void TimelineView::Duplicate(tas::Replay& r)
{
    if (Empty()) return;
    const tas::timeline::Clip clip = tas::timeline::Copy(r, holds_, cams_);
    std::uint32_t first = UINT32_MAX;
    for (const KeyHold& k : holds_) first = std::min(first, k.hold.start);
    for (std::uint32_t f : cams_) first = std::min(first, f);
    History().Push(r, "Duplicate " + Count(holds_.size() + cams_.size(), "item", "items"));
    cams_.clear();
    holds_ = tas::timeline::Paste(r, clip, first + clip.length, &cams_);    // Right after the original
}

// Where a paste goes: the frame under the mouse, else the session's frame, else where the selection starts.
std::uint32_t TimelineView::PasteFrame(const tas::Status& st) const
{
    if (hoveredCanvas_) return static_cast<std::uint32_t>(mouseFrame_);
    if (st.mode != tas::Mode::Idle) return static_cast<std::uint32_t>(st.frame);
    std::uint32_t first = 0;
    if (!holds_.empty()) first = holds_.front().hold.start;
    return first;
}

Hold TimelineView::ResizePreview() const
{
    Hold h = drag_.one.hold;
    if (drag_.what == Drag::What::Start)
        h.start = static_cast<std::uint32_t>(std::clamp(static_cast<int>(h.start) + drag_.delta, 0, static_cast<int>(h.end) - 1));
    else if (drag_.what == Drag::What::End)
        h.end = static_cast<std::uint32_t>(std::max(static_cast<int>(h.start) + 1, static_cast<int>(h.end) + drag_.delta));
    return h;
}

void TimelineView::Commit(tas::Replay& r)
{
    switch (drag_.what) {
        case Drag::What::Move:
            MoveSelection(r, drag_.delta);
            break;
        case Drag::What::Start:
        case Drag::What::End: {
            if (drag_.delta == 0) break;
            History().Push(r, "Resize " + r.Name(drag_.one.name));
            const Hold h = ResizePreview();
            auto holds = drag_.others;
            holds.push_back(h);
            tas::timeline::SetHolds(r, drag_.one.name, holds);
            holds_ = {{drag_.one.name, tas::timeline::Containing(r, drag_.one.name, h)}};
            cams_.clear();
            break;
        }
        default: break;
    }
    drag_ = {};
}

// ---- the canvas ------------------------------------------------------------------------------------------------------------
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
    viewW_ = viewW;

    if (fitNext_) {
        fitNext_ = false;
        pxPerFrame_ = std::clamp((viewW - kLabelW - 30.f) / static_cast<float>(std::max<std::uint32_t>(frameCount, 10)),
                                 kMinZoom, kMaxZoom);
        ImGui::SetScrollX(0.f);
    }
    // Ctrl + wheel: zoom with the frame under the mouse staying put
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.f) {
        const float mouseOff = io.MousePos.x - winPos.x - kLabelW;
        const float anchor = (ImGui::GetScrollX() + mouseOff) / pxPerFrame_;
        pxPerFrame_ = std::clamp(pxPerFrame_ * std::pow(1.2f, io.MouseWheel), kMinZoom, kMaxZoom);
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
    const auto RowAt = [&](float y) { return static_cast<int>(std::floor((y - rowsY) / kRowH)); };

    ImGui::InvisibleButton("##hit", {std::max(totalW, viewW - 4.f), std::max(totalH, 10.f)},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 m = io.MousePos;
    const int mouseFrame = FrameAt(m.x);
    const bool inLabels = m.x < winPos.x + kLabelW;
    hoveredCanvas_ = hovered && !inLabels;
    mouseFrame_ = mouseFrame;

    // ---- what is under the mouse ----------------------------------------------------------------------------------------------
    struct Hit {
        enum class Kind { None, Ruler, Camera, CameraKey, Track, Hold } kind = Kind::None;
        int track = -1;
        Hold hold;
        Drag::What edge = Drag::What::Move;
        std::uint32_t camFrame = 0;
        int mark = -1;                          // Ruler: the bookmark under the mouse (its index)
    } hit;
    if (hovered && !inLabels) {
        if (m.y < cameraY) {
            hit.kind = Hit::Kind::Ruler;
            for (std::size_t i = 0; i < r.markers.size(); ++i)
                if (std::abs(X(r.markers[i].frame + 0.5) - m.x) <= kKeyHit) hit.mark = static_cast<int>(i);
        } else if (m.y < rowsY) {
            hit.kind = Hit::Kind::Camera;
            for (const tas::CameraKey& k : r.camera)
                if (std::abs(X(k.frame + 0.5) - m.x) <= kKeyHit) { hit.kind = Hit::Kind::CameraKey; hit.camFrame = k.frame; }
        } else {
            const int row = RowAt(m.y);
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
    const auto markFrame = [&](int i) { return r.markers[static_cast<std::size_t>(i)].frame; };
    if (hit.kind == Hit::Kind::Hold && hit.edge != Drag::What::Move && !readOnly)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (drag_.what == Drag::What::Start || drag_.what == Drag::What::End) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // ---- mouse ------------------------------------------------------------------------------------------------------------------
    const bool editable = !readOnly;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && editable) {
        if (hit.kind == Hit::Kind::Ruler && hit.mark < 0 && mouseFrame < static_cast<int>(frameCount)) {
            History().Push(r, "Add a bookmark");
            r.SetMarker(static_cast<std::uint32_t>(mouseFrame), "Bookmark " + std::to_string(r.markers.size() + 1));
        } else if (hit.kind == Hit::Kind::Camera && mouseFrame < static_cast<int>(frameCount)) {
            // A new key where the camera already is on that frame (or the nearest key's direction)
            tas::CameraKey k;
            k.frame = static_cast<std::uint32_t>(mouseFrame);
            if (!tas::timeline::CameraAt(r, k.frame, k.yaw, k.pitch) && !r.camera.empty()) {
                const tas::CameraKey& closest = k.frame < r.camera.front().frame ? r.camera.front() : r.camera.back();
                k.yaw = closest.yaw;
                k.pitch = closest.pitch;
            }
            History().Push(r, "Add a camera key");
            tas::timeline::SetCameraKey(r, k);
            SelectCamera(k.frame, false);
        } else if (hit.kind == Hit::Kind::Track && !tracks[hit.track].axis) {
            const Track& t = tracks[hit.track];
            auto holds = t.holds;
            const Hold h{static_cast<std::uint32_t>(mouseFrame), static_cast<std::uint32_t>(mouseFrame + newHold_)};
            holds.push_back(h);
            History().Push(r, "Add " + r.Name(t.name));
            tas::timeline::SetHolds(r, t.name, holds);
            Select({t.name, tas::timeline::Containing(r, t.name, h)}, false);
        }
        drag_ = {};
    } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_ = {};
        drag_.startX = m.x;
        drag_.startY = m.y;
        const bool ctrl = io.KeyCtrl;
        if (io.KeyShift && hit.kind != Hit::Kind::Ruler) {
            drag_.what = Drag::What::Box;               // Shift + drag: select a box (ctrl too: add to the selection)
            drag_.additive = ctrl;
        } else {
            switch (hit.kind) {
                case Hit::Kind::Ruler:
                    if (editable) drag_.what = Drag::What::Seek;
                    break;
                case Hit::Kind::CameraKey:
                    if (ctrl) { SelectCamera(hit.camFrame, true); break; }
                    if (!SelectedCamera(hit.camFrame)) SelectCamera(hit.camFrame, false);
                    if (editable) drag_.what = Drag::What::Move;
                    break;
                case Hit::Kind::Hold: {
                    const KeyHold k{tracks[hit.track].name, hit.hold};
                    if (ctrl) { Select(k, true); break; }
                    if (hit.edge != Drag::What::Move) {
                        Select(k, false);                // Resizing is one bar at a time
                        if (!editable) break;
                        drag_.what = hit.edge;
                        drag_.one = k;
                        for (const Hold& h : tracks[hit.track].holds)
                            if (!(h == hit.hold)) drag_.others.push_back(h);
                        break;
                    }
                    if (!Selected(k)) Select(k, false);  // Dragging a selected bar moves the whole selection
                    if (editable) drag_.what = Drag::What::Move;
                    break;
                }
                default:
                    if (!ctrl) { holds_.clear(); cams_.clear(); }
                    drag_.what = Drag::What::Box;        // A drag over empty space selects a box too
                    drag_.additive = ctrl;
                    break;
            }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ctx_ = {};
        ctx_.frame = static_cast<std::uint32_t>(mouseFrame);
        switch (hit.kind) {
            case Hit::Kind::Hold:
                ctx_.kind = Context::Kind::Hold;
                ctx_.hold = {tracks[hit.track].name, hit.hold};
                if (!Selected(ctx_.hold)) Select(ctx_.hold, false);
                break;
            case Hit::Kind::CameraKey:
                ctx_.kind = Context::Kind::Camera;
                ctx_.frame = hit.camFrame;
                if (!SelectedCamera(hit.camFrame)) SelectCamera(hit.camFrame, false);
                break;
            case Hit::Kind::Track:
                ctx_.kind = Context::Kind::Row;
                ctx_.row = tracks[hit.track].name;
                ctx_.axis = tracks[hit.track].axis;
                break;
            case Hit::Kind::Ruler:
            case Hit::Kind::Camera:
                ctx_.kind = hit.mark >= 0 ? Context::Kind::Mark : Context::Kind::Ruler;
                if (hit.mark >= 0) {
                    ctx_.frame = markFrame(hit.mark);
                    std::snprintf(markName_, sizeof markName_, "%s", r.markers[static_cast<std::size_t>(hit.mark)].name.c_str());
                }
                break;
            default: break;
        }
        if (ctx_.kind != Context::Kind::None) ImGui::OpenPopup("##timeline-menu");
    }
    if (drag_.what != Drag::What::None && ImGui::IsItemActive())
        drag_.delta = static_cast<int>(std::lround((m.x - drag_.startX) / ppf));
    if (drag_.what != Drag::What::None && ImGui::IsItemDeactivated()) {
        if (drag_.what == Drag::What::Seek) {
            // A click on a bookmark's flag goes exactly to it
            const int to = hit.mark >= 0 ? static_cast<int>(markFrame(hit.mark)) : mouseFrame;
            tas::Goto(static_cast<std::size_t>(std::min(to, static_cast<int>(frameCount))));
            drag_ = {};
        } else if (drag_.what == Drag::What::Box) {
            // Everything the box touches: holds in its rows and frames, camera keys if it covers the camera row
            const float y0 = std::min(drag_.startY, m.y), y1 = std::max(drag_.startY, m.y);
            const int fa = FrameAt(std::min(drag_.startX, m.x)), fb = FrameAt(std::max(drag_.startX, m.x));
            if (!drag_.additive) { holds_.clear(); cams_.clear(); }
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                const float ry0 = rowsY + static_cast<float>(i) * kRowH, ry1 = ry0 + kRowH;
                if (ry1 < y0 || ry0 > y1) continue;
                for (const Hold& h : tracks[i].holds)
                    if (static_cast<int>(h.end) > fa && static_cast<int>(h.start) <= fb && !Selected({tracks[i].name, h}))
                        holds_.push_back({tracks[i].name, h});
            }
            if (y0 < rowsY && y1 > cameraY)
                for (const tas::CameraKey& k : r.camera)
                    if (static_cast<int>(k.frame) >= fa && static_cast<int>(k.frame) <= fb && !SelectedCamera(k.frame))
                        cams_.push_back(k.frame);
            drag_ = {};
        } else {
            Commit(r);
        }
    }
    DrawContextMenu(r, readOnly);

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
    const int moveDelta = drag_.what == Drag::What::Move ? GroupDelta(r, drag_.delta) : 0;

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

    // Bookmarks: a flag on the ruler with its name, and a faint line down through the rows
    const ImU32 markCol = IM_COL32(255, 210, 80, 255);
    for (const tas::Marker& mk : r.markers) {
        if (static_cast<int>(mk.frame) < f0 - 200 || static_cast<int>(mk.frame) > f1) continue;
        const float x = X(mk.frame + 0.5);
        dl->AddLine({x, origin.y}, {x, origin.y + totalH}, IM_COL32(255, 210, 80, 70));
        dl->AddTriangleFilled({x, origin.y + 2.f}, {x + 9.f, origin.y + 6.f}, {x, origin.y + 10.f}, markCol);
        dl->AddText({x + 11.f, origin.y + 1.f}, markCol, mk.name.c_str());
    }

    // Camera: the range the keys drive, and the keys as diamonds
    if (!r.camera.empty()) {
        const float y = cameraY + kCameraH * 0.5f;
        dl->AddRectFilled({X(r.camera.front().frame + 0.5), y - 2.f}, {X(r.camera.back().frame + 0.5), y + 2.f},
                          ImGui::GetColorU32(ImGuiCol_PlotLines, 0.5f), 2.f);
        for (const tas::CameraKey& k : r.camera) {
            const bool selected = SelectedCamera(k.frame);
            const float x = X(static_cast<double>(static_cast<int>(k.frame) + (selected ? moveDelta : 0)) + 0.5);
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
            // Movement as bars from the middle: up for positive, down for negative, as tall as the biggest in view
            const ImU32 col = TrackColor(name, 0.85f);
            const float mid = (y0 + y1) * 0.5f, half = (y1 - y0) * 0.5f;
            dl->AddLine({left, mid}, {right, mid}, ImGui::GetColorU32(ImGuiCol_Border, 0.5f));
            auto from = std::lower_bound(t.moves.begin(), t.moves.end(), static_cast<std::uint32_t>(f0));
            std::vector<std::pair<std::uint32_t, float>> shown;
            float biggest = 0.f;
            for (auto it = from; it != t.moves.end() && static_cast<int>(*it) <= f1; ++it) {
                const float v = tas::timeline::AxisAt(r, *it, t.name);
                shown.push_back({*it, v});
                biggest = std::max(biggest, std::fabs(v));
            }
            for (const auto& [f, v] : shown) {
                const float x = X(f), w = std::max(1.f, ppf - 1.f);
                const float hgt = biggest > 0.f ? std::max(1.f, half * std::fabs(v) / biggest) : 1.f;
                dl->AddRectFilled({x, v >= 0.f ? mid - hgt : mid}, {x + w, v >= 0.f ? mid : mid + hgt}, col);
            }
            continue;
        }
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
        const bool resizing = (drag_.what == Drag::What::Start || drag_.what == Drag::What::End) && drag_.one.name == t.name;
        for (const Hold& h : t.holds) {
            const KeyHold k{t.name, h};
            const bool selected = Selected(k);
            if (resizing && h == drag_.one.hold) continue;
            if (selected && moveDelta != 0)
                drawHold({static_cast<std::uint32_t>(static_cast<int>(h.start) + moveDelta),
                          static_cast<std::uint32_t>(static_cast<int>(h.end) + moveDelta)}, true);
            else drawHold(h, selected);
        }
        if (resizing) drawHold(ResizePreview(), true);
    }

    // The box being dragged
    if (drag_.what == Drag::What::Box && ImGui::IsItemActive()) {
        const ImVec2 a{std::min(drag_.startX, m.x), std::min(drag_.startY, m.y)};
        const ImVec2 b{std::max(drag_.startX, m.x), std::max(drag_.startY, m.y)};
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_DragDropTarget, 0.15f));
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_DragDropTarget, 0.8f));
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

    if (hovered && hit.kind != Hit::Kind::None && drag_.what == Drag::What::None && !ImGui::IsPopupOpen("##timeline-menu")) {
        if (hit.kind == Hit::Kind::Hold)
            ImGui::SetTooltip("%s: frames %u-%u (%u)", r.Name(tracks[hit.track].name).c_str(), hit.hold.start,
                              hit.hold.end - 1, hit.hold.Length());
        else if (hit.kind == Hit::Kind::CameraKey)
            ImGui::SetTooltip("Camera key on frame %u", hit.camFrame);
        else if (hit.kind == Hit::Kind::Camera && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Double click: a camera key on frame %d", mouseFrame);
        else if (hit.mark >= 0)
            ImGui::SetTooltip("%s (frame %u)\nClick: go there. Right click: rename, delete",
                              r.markers[static_cast<std::size_t>(hit.mark)].name.c_str(), markFrame(hit.mark));
        else if (hit.kind == Hit::Kind::Ruler && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Click: go to frame %d (%s). Double click: a bookmark. Right click: insert / delete frames",
                              mouseFrame, FrameTime(mouseFrame, r.fixedHz).c_str());
    }

    ImGui::EndChild();
}

// ---- right click ----------------------------------------------------------------------------------------------------------
void TimelineView::DrawContextMenu(tas::Replay& r, bool readOnly)
{
    if (!ImGui::BeginPopup("##timeline-menu")) return;
    const auto frames = static_cast<std::uint32_t>(r.frames.size());
    const std::uint32_t f = ctx_.frame;
    const std::size_t n = holds_.size() + cams_.size();
    char label[96];
    ImGui::BeginDisabled(readOnly);

    switch (ctx_.kind) {
        case Context::Kind::Hold: {
            const KeyHold& k = ctx_.hold;
            ImGui::TextDisabled("%s, frames %u-%u", r.Name(k.name).c_str(), k.hold.start, k.hold.end - 1);
            ImGui::Separator();
            // Let go on the frame under the mouse: two holds with a one frame gap
            std::snprintf(label, sizeof label, "Split at frame %u", f);
            if (ImGui::MenuItem(label, nullptr, false, f > k.hold.start && f + 1 < k.hold.end)) {
                History().Push(r, "Split " + r.Name(k.name));
                tas::timeline::SetHeld(r, k.name, f, false);
                holds_.clear();
            }
            if (ImGui::MenuItem(n > 1 ? "Duplicate selection" : "Duplicate", "Ctrl+D")) Duplicate(r);
            if (ImGui::MenuItem(n > 1 ? "Copy selection" : "Copy", "Ctrl+C")) CopySelection(r);
            if (ImGui::MenuItem("Select the whole row")) {
                holds_.clear();
                cams_.clear();
                for (const Hold& h : tas::timeline::Holds(r, k.name)) holds_.push_back({k.name, h});
            }
            ImGui::Separator();
            if (ImGui::MenuItem(n > 1 ? "Delete selection" : "Delete", "Del")) DeleteSelection(r);
            break;
        }
        case Context::Kind::Camera: {
            const tas::CameraKey* key = tas::timeline::FindCameraKey(r, f);
            if (!key) break;
            ImGui::TextDisabled("Camera key, frame %u", f);
            ImGui::Separator();
            static const char* eases[] = {"Linear", "Smooth", "Hold"};
            for (int e = 0; e < 3; ++e)
                if (ImGui::MenuItem(eases[e], nullptr, static_cast<int>(key->ease) == e)) {
                    History().Push(r, "Change ease");
                    for (std::uint32_t cf : cams_.empty() ? std::vector<std::uint32_t>{f} : cams_)
                        if (const tas::CameraKey* c = tas::timeline::FindCameraKey(r, cf)) {
                            tas::CameraKey changed = *c;
                            changed.ease = static_cast<tas::Ease>(e);
                            tas::timeline::SetCameraKey(r, changed);
                        }
                }
            ImGui::Separator();
            if (ImGui::MenuItem("Go to this frame")) tas::Goto(f);
            if (ImGui::MenuItem("Take the current view")) {
                History().Push(r, "Camera key from the view");
                tas::KeyCameraFromView(f);
            }
            ImGui::SetItemTooltip("The look direction the player has right now (go to this frame first)");
            if (ImGui::MenuItem(n > 1 ? "Duplicate selection" : "Duplicate", "Ctrl+D")) Duplicate(r);
            if (ImGui::MenuItem(n > 1 ? "Copy selection" : "Copy", "Ctrl+C")) CopySelection(r);
            ImGui::Separator();
            if (ImGui::MenuItem(n > 1 ? "Delete selection" : "Delete", "Del")) DeleteSelection(r);
            break;
        }
        case Context::Kind::Mark: {
            ImGui::TextDisabled("Bookmark, frame %u", f);
            ImGui::SetNextItemWidth(180.f);
            if (ImGui::InputText("##mark", markName_, sizeof markName_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                History().Push(r, "Rename a bookmark");
                r.SetMarker(f, markName_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Go to this frame")) tas::Goto(f);
            if (ImGui::MenuItem("Delete")) {
                History().Push(r, "Delete a bookmark");
                r.EraseMarker(f);
            }
            break;
        }
        case Context::Kind::Row:
        case Context::Kind::Ruler: {
            ImGui::TextDisabled("Frame %u", f);
            ImGui::Separator();
            if (ctx_.kind == Context::Kind::Row && !ctx_.axis) {
                std::snprintf(label, sizeof label, "Hold %s here (%d frames)", r.Name(ctx_.row).c_str(), newHold_);
                if (ImGui::MenuItem(label)) {
                    auto holds = tas::timeline::Holds(r, ctx_.row);
                    const Hold h{f, f + static_cast<std::uint32_t>(newHold_)};
                    holds.push_back(h);
                    History().Push(r, "Add " + r.Name(ctx_.row));
                    tas::timeline::SetHolds(r, ctx_.row, holds);
                    Select({ctx_.row, tas::timeline::Containing(r, ctx_.row, h)}, false);
                }
                if (ImGui::MenuItem("Select the whole row")) {
                    holds_.clear();
                    cams_.clear();
                    for (const Hold& h : tas::timeline::Holds(r, ctx_.row)) holds_.push_back({ctx_.row, h});
                }
            }
            if (ImGui::MenuItem("Paste here", "Ctrl+V", false, !clip_.Empty())) PasteAt(r, f);
            if (ImGui::MenuItem("Go to this frame", nullptr, false, f <= frames)) tas::Goto(f);
            if (ImGui::MenuItem("Bookmark this frame", nullptr, false, f < frames)) {
                History().Push(r, "Add a bookmark");
                r.SetMarker(f, "Bookmark " + std::to_string(r.markers.size() + 1));
            }
            ImGui::Separator();
            // Frames in or out: everything after moves along (inputs, camera keys, bookmarks)
            ImGui::SetNextItemWidth(110.f);
            ImGui::InputInt("frames", &frameCount_, 1, 10);
            frameCount_ = std::clamp(frameCount_, 1, 100000);
            std::snprintf(label, sizeof label, "Insert %d before frame %u", frameCount_, f);
            if (ImGui::MenuItem(label, nullptr, false, f <= frames)) {
                History().Push(r, "Insert " + Count(static_cast<std::size_t>(frameCount_), "frame", "frames"));
                r.InsertFrames(std::min(f, frames), static_cast<std::size_t>(frameCount_), NominalDt(r, f));
                holds_.clear();
                cams_.clear();
            }
            const auto cut = std::min<std::uint32_t>(static_cast<std::uint32_t>(frameCount_), frames > f ? frames - f : 0);
            std::snprintf(label, sizeof label, "Delete frames %u-%u", f, f + std::max<std::uint32_t>(cut, 1) - 1);
            if (ImGui::MenuItem(label, nullptr, false, cut > 0)) {
                History().Push(r, "Delete " + Count(cut, "frame", "frames"));
                r.EraseFrames(f, cut);
                holds_.clear();
                cams_.clear();
            }
            break;
        }
        default: break;
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

// ---- the selection, below the timeline -------------------------------------------------------------------------------------
void TimelineView::DrawDetails(tas::Replay& r, bool readOnly)
{
    ImGui::BeginDisabled(readOnly);
    if (holds_.size() == 1 && cams_.empty()) {
        const KeyHold sel = holds_.front();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(r.Name(sel.name).c_str());
        ImGui::SameLine();
        int start = static_cast<int>(sel.hold.start), length = static_cast<int>(sel.hold.Length());
        // Applied on Enter or the +/- buttons, not on every typed digit (a half typed number would merge holds)
        ImGui::SetNextItemWidth(110.f);
        bool apply = ImGui::InputInt("from frame", &start, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.f);
        apply |= ImGui::InputInt("frames held", &length, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
        if (apply) {
            std::vector<Hold> holds;
            for (const Hold& h : tas::timeline::Holds(r, sel.name))
                if (!(h == sel.hold)) holds.push_back(h);
            const Hold h{static_cast<std::uint32_t>(std::max(0, start)),
                         static_cast<std::uint32_t>(std::max(0, start) + std::max(1, length))};
            holds.push_back(h);
            History().Push(r, "Edit " + r.Name(sel.name));
            tas::timeline::SetHolds(r, sel.name, holds);
            holds_ = {{sel.name, tas::timeline::Containing(r, sel.name, h)}};
        }
        ImGui::SameLine();
        if (ImGui::Button("Go to")) tas::Goto(sel.hold.start);
        ImGui::SameLine();
        if (ImGui::Button("Delete")) DeleteSelection(r);
    } else if (cams_.size() == 1 && holds_.empty()) {
        tas::CameraKey k = *tas::timeline::FindCameraKey(r, cams_.front());
        bool changed = false;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Camera key, frame %u", k.frame);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        changed |= ImGui::DragFloat("yaw", &k.yaw, 0.5f, 0.f, 0.f, "%.1f deg");
        if (ImGui::IsItemActivated()) History().Push(r, "Edit a camera key");
        ImGui::SetItemTooltip("Not wrapped: 370 is a full turn further than 10, so the camera turns all the way round");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.f);
        changed |= ImGui::DragFloat("pitch", &k.pitch, 0.5f, -89.f, 89.f, "%.1f deg");
        if (ImGui::IsItemActivated()) History().Push(r, "Edit a camera key");
        ImGui::SameLine();
        static const char* eases[] = {"Linear", "Smooth", "Hold"};
        int ease = static_cast<int>(k.ease);
        ImGui::SetNextItemWidth(90.f);
        if (ImGui::Combo("to next", &ease, eases, 3)) {
            History().Push(r, "Change ease");
            k.ease = static_cast<tas::Ease>(ease);
            changed = true;
        }
        if (changed) tas::timeline::SetCameraKey(r, k);
        ImGui::SameLine();
        if (ImGui::Button("From camera")) {
            History().Push(r, "Camera key from the view");
            tas::KeyCameraFromView(k.frame);
        }
        ImGui::SetItemTooltip("Take the look direction the player has right now (go to this frame first)");
        ImGui::SameLine();
        if (ImGui::Button("Go to")) tas::Goto(k.frame);
        ImGui::SameLine();
        if (ImGui::Button("Delete")) DeleteSelection(r);
    } else if (!Empty()) {
        ImGui::AlignTextToFramePadding();
        std::string what = holds_.empty() ? "" : Count(holds_.size(), "hold", "holds");
        if (!cams_.empty()) what += (what.empty() ? "" : ", ") + Count(cams_.size(), "camera key", "camera keys");
        ImGui::Text("%s selected", what.c_str());
        ImGui::SameLine();
        if (ImGui::ArrowButton("##left", ImGuiDir_Left)) MoveSelection(r, ImGui::GetIO().KeyShift ? -10 : -1);
        ImGui::SetItemTooltip("A frame earlier (shift: 10). Left arrow key");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##right", ImGuiDir_Right)) MoveSelection(r, ImGui::GetIO().KeyShift ? 10 : 1);
        ImGui::SetItemTooltip("A frame later (shift: 10). Right arrow key");
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) Duplicate(r);
        ImGui::SameLine();
        if (ImGui::Button("Copy")) CopySelection(r);
        ImGui::SameLine();
        if (ImGui::Button("Delete")) DeleteSelection(r);
    } else {
        ImGui::TextDisabled("Click to select, shift + drag for a box, double click to add, right click for more. "
                            "Ctrl+Z undoes. (?) above lists the keys.");
    }
    if (!clip_.Empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| clipboard: %s", Count(clip_.holds.size() + clip_.camera.size(), "item", "items").c_str());
    }
    ImGui::EndDisabled();
}

} // namespace tasui
