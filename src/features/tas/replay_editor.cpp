#include "features/tas/replay_editor.h"
#include "features/tas/timeline_view.h"      // KeyColor
#include "tas/timeline.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace tasui {

using tas::timeline::Hold;
using tas::timeline::Track;

namespace {

bool Running(const tas::Status& st) { return st.mode != tas::Mode::Idle && !st.armed; }

float NominalDt(const tas::Replay& r) { return static_cast<float>(1.0 / (r.fixedHz > 0.f ? r.fixedHz : 60.f)); }

int FindName(const tas::Replay& r, const std::string& name)
{
    for (std::size_t i = 0; i < r.names.size(); ++i)
        if (r.names[i] == name) return static_cast<int>(i);
    return -1;
}

// "W SpaceBar" for the frame list: the keys held on a frame
std::string HeldKeys(const tas::Replay& r, const std::vector<Track>& tracks, std::uint32_t frame)
{
    std::string out;
    for (const Track& t : tracks) {
        if (t.axis || !tas::timeline::HeldAt(t.holds, frame)) continue;
        if (!out.empty()) out += ' ';
        out += r.Name(t.name);
    }
    return out;
}

const char* kNoBack = "A macro that starts \"now\" can't go back: the game can't return to where it began.\n"
                      "Record from a level start to go back and forth.";

} // namespace

void ReplayEditor::Draw(bool& open, const tas::Status& st, const EditorKeys& keys)
{
    ImGui::SetNextWindowSize({720.f, 480.f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Replay Editor", &open)) {
        ImGui::End();
        return;
    }
    tas::WithReplay([&](tas::Replay& r) {
        History().Watch(r);     // Shared with the timeline: an edit here can be undone there and the other way round
        const auto count = static_cast<long long>(r.frames.size());
        if (follow_ && Running(st) && count > 0) selected_ = std::min(static_cast<long long>(st.frame), count - 1);
        if (selected_ >= count) selected_ = count - 1;
        if (selected_ < 0 && count > 0) selected_ = 0;

        DrawTransport(r, st, keys);
        ImGui::Separator();
        const float listWidth = ImGui::GetContentRegionAvail().x * 0.5f;
        ImGui::BeginChild("frames", {listWidth, 0}, ImGuiChildFlags_Borders);
        DrawFrameList(r, st);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("frame", {0, 0}, ImGuiChildFlags_Borders);
        DrawInputs(r, st);
        ImGui::EndChild();
    });
    ImGui::End();
}

// |< < Pause > >| and Go to: these move the game, not just the selection. Going back plays the macro again from its
// start (as fast as it can) and pauses on the frame.
void ReplayEditor::DrawTransport(tas::Replay& r, const tas::Status& st, const EditorKeys& keys)
{
    const bool running = Running(st);
    const std::size_t count = r.frames.size();
    const std::size_t at = running ? st.frame : static_cast<std::size_t>(std::max(0ll, selected_));
    const bool canBack = r.start != tas::StartKind::Now || !running;
    const bool recording = st.mode == tas::Mode::Recording;
    const char* stopsRecording = recording ? "\nStops recording: Take over on that frame to record from there" : "";
    const auto go = [&](std::size_t frame) {
        frame = std::min(frame, count);
        tas::Goto(frame);
        if (count) selected_ = static_cast<long long>(std::min(frame, count - 1));
    };

    ImGui::BeginDisabled(count == 0);
    ImGui::BeginDisabled(!canBack);
    if (ImGui::Button("|<")) go(0);
    ImGui::SetItemTooltip("%s", canBack ? (std::string("Back to the start: frame 0") + stopsRecording).c_str() : kNoBack);
    ImGui::SameLine();
    if (ImGui::Button(" < ")) go(at > 0 ? at - 1 : 0);
    ImGui::SetItemTooltip("%s", canBack ? (std::string("One frame back (plays the macro again up to there)") +
                                           stopsRecording).c_str()
                                        : kNoBack);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(running ? (st.paused ? "Resume" : "Pause") : "Play", {64.f, 0})) {
        if (running) tas::TogglePause();
        else tas::Play();
    }
    ImGui::SetItemTooltip("%s", running ? keys.pause.c_str() : "Play the macro from its start");
    ImGui::SameLine();
    if (ImGui::Button(" > ")) {
        if (running) tas::Advance(1);
        else go(at + 1);
    }
    ImGui::SetItemTooltip("One frame on (%s)", keys.advance.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!canBack && at >= count);
    if (ImGui::Button(">|")) go(count);
    ImGui::SetItemTooltip("To the end of the macro");
    ImGui::EndDisabled();

    // Go to any frame
    ImGui::SameLine(0.f, 16.f);
    ImGui::SetNextItemWidth(80.f);
    const bool enter = ImGui::InputInt("##goto", &goto_, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
    goto_ = std::clamp(goto_, 0, static_cast<int>(count));
    ImGui::SameLine();
    const bool back = running && static_cast<std::size_t>(goto_) < at;
    ImGui::BeginDisabled(back && !canBack);
    if (ImGui::Button("Go to") || enter) go(static_cast<std::size_t>(goto_));
    ImGui::SetItemTooltip("%s", back && !canBack ? kNoBack
                                                 : (std::string("Play the macro up to this frame and pause there, in "
                                                                "the game too") + stopsRecording).c_str());
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // Paused at the end of a playback: grow the macro by a frame and play it
    if (st.mode == tas::Mode::Playing && running && st.paused && st.frame >= count) {
        ImGui::SameLine();
        if (ImGui::Button("+ Frame & step")) {
            History().Push(r, "Add a frame");
            r.InsertFrames(r.frames.size(), 1, r.frames.empty() ? NominalDt(r) : r.frames.back().dt);
            selected_ = static_cast<long long>(r.frames.size()) - 1;
            tas::Advance(1);
        }
        ImGui::SetItemTooltip("Add a frame at the end (keys still held stay held), set its inputs on the right, and "
                              "play it. Builds a macro one frame at a time");
    }

    ImGui::SameLine(0.f, 16.f);
    ImGui::Checkbox("Follow", &follow_);
    ImGui::SetItemTooltip("Select the frame that plays next while a macro runs");

    // Where it is
    char where[96];
    if (!running && st.armed) std::snprintf(where, sizeof where, "loading the start level...");
    else if (!running) std::snprintf(where, sizeof where, "%zu frames, not running", count);
    else std::snprintf(where, sizeof where, "%s frame %zu of %zu%s", recording ? "REC" : st.seeking ? "SEEK" : "PLAY",
                       st.frame, count, st.paused ? ", paused" : "");
    ImGui::TextDisabled("%s", where);
    if (r.start == tas::StartKind::Now) {
        ImGui::SameLine();
        ImGui::TextDisabled("| starts \"now\": can only go forward while it runs");
    }
}

void ReplayEditor::DrawFrameList(tas::Replay& r, const tas::Status& st)
{
    const std::vector<Track> tracks = tas::timeline::Tracks(r);
    const int mouseX = FindName(r, "MouseX"), mouseY = FindName(r, "MouseY");
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("frames", 4, kFlags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Frame");
    ImGui::TableSetupColumn("ms");
    ImGui::TableSetupColumn("Held", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Mouse");
    ImGui::TableHeadersRow();

    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const bool running = Running(st);
    if (follow_ && running && st.frame != lastFollowed_) {
        ImGui::SetScrollY(std::max(0.f, (static_cast<float>(st.frame) - 5.f) * rowH));
        lastFollowed_ = st.frame;
    }

    ImGuiListClipper clipper;      // only the visible rows are built: hours of frames cost nothing
    clipper.Begin(static_cast<int>(r.frames.size()), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const tas::Frame& f = r.frames[static_cast<std::size_t>(i)];
            ImGui::TableNextRow();
            if (running && static_cast<std::size_t>(i) == st.frame)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 110, 200, 120));
            if (st.desyncFrame == i) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(200, 60, 50, 140));

            ImGui::TableNextColumn();
            char label[16];
            std::snprintf(label, sizeof label, "%d", i);
            if (ImGui::Selectable(label, selected_ == i, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_ = i;
                follow_ = false;            // picked by hand: stay there
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) tas::Goto(i);
            ImGui::SetItemTooltip("Double click: go to this frame in the game");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", f.dt * 1000.f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(HeldKeys(r, tracks, static_cast<std::uint32_t>(i)).c_str());
            ImGui::TableNextColumn();
            const float dx = mouseX >= 0 ? tas::timeline::AxisAt(r, i, static_cast<std::uint16_t>(mouseX)) : 0.f;
            const float dy = mouseY >= 0 ? tas::timeline::AxisAt(r, i, static_cast<std::uint16_t>(mouseY)) : 0.f;
            if (dx != 0.f || dy != 0.f) ImGui::TextDisabled("%+.1f %+.1f", dx, dy);
        }
    }
    ImGui::EndTable();
}

// What is held on the selected frame, as keys you click to change; the mouse; the frame itself.
void ReplayEditor::DrawInputs(tas::Replay& r, const tas::Status& st)
{
    const std::size_t count = r.frames.size();
    const bool readOnly = st.mode == tas::Mode::Recording;
    ImGui::BeginDisabled(readOnly);

    if (count == 0 || selected_ < 0) {
        ImGui::TextDisabled("No frames yet: record or load a macro, or add frames");
        if (ImGui::Button("+ Frames")) {
            History().Push(r, "Add frames");
            r.InsertFrames(0, static_cast<std::size_t>(frameCount_), NominalDt(r));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.f);
        ImGui::InputInt("##n", &frameCount_, 0, 0);
        frameCount_ = std::clamp(frameCount_, 1, 100000);
        ImGui::EndDisabled();
        return;
    }
    const auto fi = static_cast<std::size_t>(selected_);
    const bool running = Running(st);

    ImGui::Text("Frame %zu", fi);
    ImGui::SameLine();
    if (running) ImGui::TextDisabled(fi == st.frame ? "(plays next)" : fi < st.frame ? "(played)" : "(later)");
    if (st.desyncFrame == static_cast<long long>(fi)) {
        ImGui::SameLine();
        ImGui::TextColored({1.f, 0.4f, 0.3f, 1.f}, "desync");
    }
    if (readOnly) {
        ImGui::SameLine();
        ImGui::TextDisabled("(read only while recording)");
    }

    // ---- keys ----------------------------------------------------------------------------------------------------------------
    ImGui::SeparatorText("Keys");
    const std::vector<Track> tracks = tas::timeline::Tracks(r);
    extraKeys_.erase(std::remove_if(extraKeys_.begin(), extraKeys_.end(),
                                    [&](const std::string& n) { return FindName(r, n) < 0; }),
                     extraKeys_.end());
    std::vector<std::uint16_t> keys;
    for (const Track& t : tracks)
        if (!t.axis) keys.push_back(t.name);
    for (const std::string& n : extraKeys_) {
        const auto idx = static_cast<std::uint16_t>(FindName(r, n));
        if (std::find(keys.begin(), keys.end(), idx) == keys.end()) keys.push_back(idx);
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float avail = ImGui::GetContentRegionAvail().x;
    float lineW = 0.f;
    for (std::uint16_t name : keys) {
        const std::string& label = r.Name(name);
        const std::vector<Hold> holds = tas::timeline::Holds(r, name);
        const bool held = tas::timeline::HeldAt(holds, static_cast<std::uint32_t>(fi));
        const float w = ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2.f;
        if (lineW > 0.f && lineW + style.ItemSpacing.x + w <= avail) ImGui::SameLine();
        else lineW = 0.f;
        lineW += (lineW > 0.f ? style.ItemSpacing.x : 0.f) + w;

        ImGui::PushID(name);
        ImGui::PushStyleColor(ImGuiCol_Button, held ? KeyColor(label) : ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, held ? KeyColor(label, 0.8f) : ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
        if (held) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 20, 255));
        if (ImGui::Button(label.c_str())) {
            History().Push(r, (held ? "Let go of " : "Hold ") + r.Name(name));
            tas::timeline::SetHeld(r, name, static_cast<std::uint32_t>(fi), !held);
        }
        ImGui::PopStyleColor(held ? 3 : 2);
        if (held) {
            for (const Hold& h : holds)
                if (fi >= h.start && fi < h.end)
                    ImGui::SetItemTooltip("Held on frames %u-%u. Click: let go on this frame", h.start, h.end - 1);
        } else {
            ImGui::SetItemTooltip("Click: hold %s on this frame", label.c_str());
        }
        ImGui::PopID();
    }
    if (keys.empty()) ImGui::TextDisabled("No keys yet: add one");
    ImGui::SetNextItemWidth(120.f);
    const bool add = ImGui::InputTextWithHint("##key", "add a key: W, SpaceBar", newKey_, sizeof newKey_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add key") || add) && newKey_[0]) {
        r.NameIndex(newKey_);
        if (std::find(extraKeys_.begin(), extraKeys_.end(), newKey_) == extraKeys_.end()) extraKeys_.emplace_back(newKey_);
        newKey_[0] = 0;
    }
    ImGui::SetItemTooltip("Unreal key names: W, A, S, D, SpaceBar, LeftShift, LeftControl, LeftMouseButton...");

    // ---- mouse / camera -------------------------------------------------------------------------------------------------------
    ImGui::SeparatorText("Mouse");
    float yaw = 0.f, pitch = 0.f;
    if (tas::timeline::CameraAt(r, fi, yaw, pitch))
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotLinesHovered),
                           "Camera keys turn the view here (yaw %.1f, pitch %.1f): mouse movement is left out", yaw, pitch);
    for (const char* axis : {"MouseX", "MouseY"}) {
        const int idx = FindName(r, axis);
        float v = idx >= 0 ? tas::timeline::AxisAt(r, fi, static_cast<std::uint16_t>(idx)) : 0.f;
        ImGui::SetNextItemWidth(120.f);
        const bool moved = ImGui::DragFloat(axis, &v, 0.05f, 0.f, 0.f, "%+.3f");
        if (ImGui::IsItemActivated()) History().Push(r, std::string("Change ") + axis);
        if (moved)
            tas::timeline::SetAxis(r, fi, idx >= 0 ? static_cast<std::uint16_t>(idx) : r.NameIndex(axis), v);
        ImGui::SetItemTooltip("Mouse movement on this frame, as the game got it (0: none)");
        if (std::string(axis) == "MouseX") ImGui::SameLine();
    }

    // ---- the frame ----------------------------------------------------------------------------------------------------------
    ImGui::SeparatorText("Frame");
    float ms = r.frames[fi].dt * 1000.f;
    ImGui::SetNextItemWidth(110.f);
    if (ImGui::InputFloat("ms long", &ms, 0.f, 0.f, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        History().Push(r, "Frame length");
        r.SetFrameDt(fi, std::clamp(ms, 0.1f, 100.f) / 1000.f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add after")) {
        History().Push(r, "Add frames");
        r.InsertFrames(fi + 1, static_cast<std::size_t>(frameCount_), r.frames[fi].dt);
        selected_ = static_cast<long long>(fi) + 1;
        follow_ = false;
    }
    ImGui::SetItemTooltip("Add frames after this one (keys held here stay held through them)");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        History().Push(r, "Delete frames");
        r.EraseFrames(fi, static_cast<std::size_t>(frameCount_));
        ImGui::EndDisabled();
        return;
    }
    ImGui::SetItemTooltip("Delete this frame (and the ones after it, up to the count)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.f);
    ImGui::InputInt("##n", &frameCount_, 0, 0);
    frameCount_ = std::clamp(frameCount_, 1, 100000);
    ImGui::SetItemTooltip("How many frames Add after / Delete add or remove");

    if (ImGui::CollapsingHeader("Raw inputs")) DrawRaw(r, fi);
    ImGui::EndDisabled();
}

// Every event of the frame as the engine gets it, editable: name, kind and value.
void ReplayEditor::DrawRaw(tas::Replay& r, std::size_t fi)
{
    const tas::Frame f = r.frames[fi];
    if (f.count == 0) ImGui::TextDisabled("No events on this frame");
    for (std::uint32_t k = 0; k < f.count; ++k) {
        ImGui::PushID(static_cast<int>(k));
        tas::Event e = r.events[f.first + k];
        bool changed = false;

        char name[48];
        std::snprintf(name, sizeof name, "%s", r.Name(e.name).c_str());
        ImGui::SetNextItemWidth(110.f);
        if (ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
            History().Push(r, "Edit an input");
            e.name = r.NameIndex(name);
            changed = true;
        }
        ImGui::SameLine();
        static const char* kinds[] = {"down", "up", "repeat", "double", "axis"};
        int kind = e.kind == tas::EventKind::Axis ? 4 : static_cast<int>(e.kind);
        ImGui::SetNextItemWidth(75.f);
        if (ImGui::Combo("##kind", &kind, kinds, 5)) {
            e.kind = kind == 4 ? tas::EventKind::Axis : static_cast<tas::EventKind>(kind);
            changed = true;
            History().Push(r, "Edit an input");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75.f);
        changed |= ImGui::DragFloat("##v", &e.value, 0.05f, 0.f, 0.f, "%.3f");
        if (ImGui::IsItemActivated()) History().Push(r, "Edit an input");     // Once per drag, not every frame of it
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            History().Push(r, "Delete an input");
            r.EraseEvent(fi, k);
            ImGui::PopID();
            break;
        }
        if (changed) r.ReplaceEvent(fi, k, e);
        ImGui::PopID();
    }

    ImGui::SetNextItemWidth(110.f);
    ImGui::InputTextWithHint("##newname", "key / MouseX", newName_, sizeof newName_);
    for (const auto& [label, kind] : {std::pair{"Down", tas::EventKind::Pressed}, std::pair{"Up", tas::EventKind::Released},
                                      std::pair{"Axis", tas::EventKind::Axis}}) {
        ImGui::SameLine();
        if (ImGui::Button(label) && newName_[0]) {
            tas::Event e;
            e.name = r.NameIndex(newName_);
            e.kind = kind;
            e.value = kind == tas::EventKind::Axis ? 0.f : 1.f;
            e.value2 = kind == tas::EventKind::Axis ? r.frames[fi].dt : 0.f;
            History().Push(r, "Add an input");
            r.InsertEvent(fi, r.frames[fi].count, e);
        }
    }
}

// Bottom left of the screen while a macro runs: every key of the macro, lit while held on the frame just played.
void ReplayEditor::DrawInputDisplay(const tas::Status& st)
{
    if (st.mode == tas::Mode::Idle || st.armed) return;
    tas::WithReplay([&](tas::Replay& r) {
        if (r.frames.empty()) return;
        const auto frame = static_cast<std::uint32_t>(std::min<std::size_t>(st.frame > 0 ? st.frame - 1 : 0,
                                                                            r.frames.size() - 1));
        const std::vector<Track> tracks = tas::timeline::Tracks(r);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float h = ImGui::GetFontSize() + 8.f, pad = 6.f, gap = 4.f, maxW = std::min(640.f, screen.x - 24.f);

        // Lay the chips out in rows first (they are drawn bottom up from the screen's edge)
        struct Chip { std::string text; ImU32 bg, fg; float w; };
        std::vector<Chip> chips;
        for (const Track& t : tracks) {
            if (t.axis) continue;
            const std::string& name = r.Name(t.name);
            const bool held = tas::timeline::HeldAt(t.holds, frame);
            chips.push_back({name, held ? KeyColor(name) : IM_COL32(0, 0, 0, 140),
                             held ? IM_COL32(15, 15, 15, 255) : IM_COL32(220, 220, 220, 160),
                             ImGui::CalcTextSize(name.c_str()).x + pad * 2.f});
        }
        const int mx = FindName(r, "MouseX"), my = FindName(r, "MouseY");
        const float dx = mx >= 0 ? tas::timeline::AxisAt(r, frame, static_cast<std::uint16_t>(mx)) : 0.f;
        const float dy = my >= 0 ? tas::timeline::AxisAt(r, frame, static_cast<std::uint16_t>(my)) : 0.f;
        if (mx >= 0 || my >= 0) {
            char m[48];
            std::snprintf(m, sizeof m, "mouse %+.1f %+.1f", dx, dy);
            const bool moving = dx != 0.f || dy != 0.f;
            chips.push_back({m, IM_COL32(0, 0, 0, 140), moving ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 120),
                             ImGui::CalcTextSize(m).x + pad * 2.f});
        }
        if (chips.empty()) return;

        std::vector<std::vector<const Chip*>> rows(1);
        float x = 0.f;
        for (const Chip& c : chips) {
            if (x > 0.f && x + c.w > maxW) { rows.emplace_back(); x = 0.f; }
            rows.back().push_back(&c);
            x += c.w + gap;
        }
        float y = screen.y - 12.f - static_cast<float>(rows.size()) * (h + gap);
        for (const auto& row : rows) {
            float cx = 12.f;
            for (const Chip* c : row) {
                dl->AddRectFilled({cx, y}, {cx + c->w, y + h}, c->bg, 4.f);
                dl->AddText({cx + pad, y + 4.f}, c->fg, c->text.c_str());
                cx += c->w + gap;
            }
            y += h + gap;
        }
    });
}

} // namespace tasui
