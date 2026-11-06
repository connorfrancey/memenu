// The TAS / Macro module: the menu side of tas/tas.h. The panel shows what the session is doing and the buttons that
// make sense right now; the macro's files are tasui::MacroLibrary, the timeline and the frame editor have their own
// windows (timeline_view, replay_editor).
#include "core/keys.h"
#include "core/module.h"
#include "core/notify.h"
#include "game/levels.h"
#include "gui.h"
#include "features/tas/macro_library.h"
#include "features/tas/replay_editor.h"
#include "features/tas/timeline_view.h"
#include "tas/splits.h"
#include "tas/tas.h"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string FormatTime(double seconds)
{
    char buf[32];
    const int m = static_cast<int>(seconds / 60.0);
    std::snprintf(buf, sizeof buf, "%d:%06.3f", m, seconds - m * 60.0);
    return buf;
}

// A difference to the reference: "-0.250", "+1:02.017". Ahead is negative.
std::string FormatDelta(double seconds)
{
    const char sign = seconds < 0.0 ? '-' : '+';
    seconds = std::fabs(seconds);
    char buf[32];
    if (seconds >= 60.0) {
        const int m = static_cast<int>(seconds / 60.0);
        std::snprintf(buf, sizeof buf, "%c%d:%06.3f", sign, m, seconds - m * 60.0);
    } else {
        std::snprintf(buf, sizeof buf, "%c%.3f", sign, seconds);
    }
    return buf;
}

ImVec4 DeltaColor(double delta)
{
    if (delta < -1e-9) return {0.4f, 1.f, 0.5f, 1.f};       // Ahead: faster than the reference
    if (delta > 1e-9) return {1.f, 0.45f, 0.4f, 1.f};
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

// "Flight" for "Split: Flight"; other bookmarks as they are.
const char* SplitLabel(const std::string& name)
{
    const std::size_t prefix = std::char_traits<char>::length(tas::splits::kSplitPrefix);
    return name.rfind(tas::splits::kSplitPrefix, 0) == 0 ? name.c_str() + prefix : name.c_str();
}

const float kRates[] = {0.f, 30.f, 50.f, 60.f, 62.f, 100.f, 120.f, 144.f, 240.f};

// Playback / recording speeds one click away (0 = as fast as possible)
const struct { const char* label; float speed; } kSpeeds[] = {
    {"1/4", 0.25f}, {"1/2", 0.5f}, {"1x", 1.f}, {"2x", 2.f}, {"4x", 4.f}, {"Max", 0.f},
};

class Tas final : public Module {
public:
    Tas() : Module("TAS / Macro", "TAS", "Record and play back macros and runs, practice mode, frame advance (.merp)", 10)
    {
        enabledByDefault = true;
        expanded = true;
        // Each section shows its controls only while it is open, some of them only when they apply.
        const auto in = [](ctl::Section& section, std::initializer_list<ctl::Setting*> controls) {
            for (ctl::Setting* c : controls) c->VisibleIf([&section] { return section.Get(); });
        };
        in(recordSection, {&start, &rate});
        command.VisibleIf([this] { return recordSection && start.Get() == 1; });
        level.VisibleIf([this] { return recordSection && start.Get() == 2; });
        realtimeCap.VisibleIf([this] { return recordSection && rate.Get() == 0; });
        onDeath.VisibleIf([this] { return recordSection && start.Get() != 0; });
        rewindSeconds.VisibleIf([this] { return recordSection && start.Get() != 0; });

        in(playSection, {&maxSpeed, &atEnd, &gotoFrame});
        speed.VisibleIf([this] { return playSection && !maxSpeed.Get(); });
        in(bookmarkSection, {&autoSplits, &bookmarks});
        in(splitsSection, {&splitsOnly, &splitTable});
        in(desyncSection, {&desyncChecks, &onDesync, &matchRecording});
        correctMax.VisibleIf([this] { return desyncSection && onDesync.Get() == 1; });
        in(filesSection, {&files});
        in(keysSection, {&recordKey, &playKey, &pauseKey, &advanceKey, &frameBackKey, &restartKey, &rewindKey,
                         &takeOverKey, &stopKey, &bookmarkKey, &prevBookmarkKey, &nextBookmarkKey});
        in(moreSection, {&blockStreaming, &seekNoRender, &showOverlay, &showInputDisplay, &showTimeline, &showEditor,
                         &details, &translator});
    }

    void OnDisable() override
    {
        tas::Stop();
        modules::SetReservedKeys({});
    }

    void OnRender() override
    {
        tas::SetOptions(Options());
        status_ = tas::GetStatus();
        ReserveKeys();

        if (showOverlay) DrawOverlay();
        if (showEditor) {
            bool open = true;
            editor_.Draw(open, status_, {keys::Name(pauseKey), keys::Name(advanceKey)});
            if (!open) showEditor.Set(false);
        }
        if (showInputDisplay) tasui::ReplayEditor::DrawInputDisplay(status_);
        if (showTimeline) {
            bool open = true;
            timeline_.Draw(open, status_);
            if (!open) showTimeline.Set(false);
        }
        if (gui::visible) library_.DrawPopups();
    }

private:
    tas::Options Options() const
    {
        tas::Options o;
        o.recordHz = kRates[std::clamp(rate.Get(), 0, static_cast<int>(std::size(kRates)) - 1)];
        o.realtimeCap = realtimeCap;
        o.speed = maxSpeed ? 0.f : speed.Get();
        o.onDesync = static_cast<tas::OnDesync>(onDesync.Get());
        o.correctMax = correctMax;
        o.matchRecording = matchRecording;
        o.gameplayMods = GameplayMods();
        o.blockStreaming = blockStreaming;
        o.seekWithoutRendering = seekNoRender;
        o.atEnd = static_cast<tas::AtEnd>(atEnd.Get());
        o.onDeath = static_cast<tas::OnDeath>(onDeath.Get());
        o.rewindSeconds = rewindSeconds;
        o.keyPause = pauseKey;
        o.keyAdvance = advanceKey;
        o.keyTakeOver = takeOverKey;
        o.keyStop = stopKey;
        o.keyRewind = rewindKey;
        o.keyRestart = restartKey;
        o.keyRecord = recordKey;
        o.keyPlay = playKey;
        o.keyBookmark = bookmarkKey;
        o.keyFrameBack = frameBackKey;
        o.keyPrevBookmark = prevBookmarkKey;
        o.keyNextBookmark = nextBookmarkKey;
        o.autoSplits = autoSplits;
        const RecordStart rs = Start();
        o.recordStart = rs.kind;
        o.recordCommand = rs.command;
        o.recordMap = rs.map;
        o.recordCheckpoint = rs.checkpoint;
        return o;
    }

    // While a session runs its keys belong to it: a module bound to the same key (No Clip's F5) must not switch too.
    // Idle, only Record and Play are taken (they start a session when the menu is closed).
    void ReserveKeys()
    {
        std::vector<int> keys;
        if (status_.mode == tas::Mode::Idle) keys = {recordKey, playKey, nextBookmarkKey};
        else keys = {pauseKey, advanceKey, restartKey, rewindKey, takeOverKey, stopKey, bookmarkKey, recordKey, playKey,
                     frameBackKey, prevBookmarkKey, nextBookmarkKey};
        keys.erase(std::remove(keys.begin(), keys.end(), 0), keys.end());
        if (keys != reserved_) {
            reserved_ = keys;
            modules::SetReservedKeys(keys);
        }
    }

    // ---- where a recording starts ------------------------------------------------------------------------------------
    struct RecordStart {
        tas::StartKind kind = tas::StartKind::Now;
        std::string command, map, checkpoint;
        bool ok = true;
    };

    RecordStart Start() const
    {
        RecordStart rs;
        rs.kind = static_cast<tas::StartKind>(start.Get());
        rs.command = commandBuf_;
        if (rs.kind == tas::StartKind::Checkpoint) {
            const auto maps = levels::Maps();
            if (map_ < 0 || map_ >= static_cast<int>(maps.size())) { rs.ok = false; return rs; }
            rs.map = maps[map_].label;
            if (checkpoint_ > 0 && checkpoint_ <= static_cast<int>(maps[map_].checkpoints.size()))
                rs.checkpoint = maps[map_].checkpoints[checkpoint_ - 1];
        }
        return rs;
    }

    void StartRecording()
    {
        const RecordStart rs = Start();
        if (!rs.ok) { notify::Push("TAS: pick a level"); return; }
        library_.Guard("Recording", [rs] { tas::Record(rs.kind, rs.command, rs.map, rs.checkpoint); });
    }

    void DrawCommand(float width)
    {
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##cmd", "open edge_p", commandBuf_, sizeof commandBuf_);
    }

    void DrawLevel(float width)
    {
        const auto maps = levels::Maps();
        if (maps.empty()) { ImGui::TextDisabled("No level list yet"); return; }
        map_ = std::clamp(map_, 0, static_cast<int>(maps.size()) - 1);
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##map", maps[map_].label.c_str())) {
            for (int i = static_cast<int>(maps.size()) - 1; i >= 0; --i)   // last to first, like the Levels tab
                if (ImGui::Selectable(maps[i].label.c_str(), i == map_)) { map_ = i; checkpoint_ = 1; }
            ImGui::EndCombo();
        }
        // Checkpoint A is the level start: the level start is only a choice of its own without checkpoints
        const auto& cps = maps[map_].checkpoints;
        checkpoint_ = std::clamp(checkpoint_, cps.empty() ? 0 : 1, static_cast<int>(cps.size()));
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##cp", checkpoint_ == 0 ? "Level start" : cps[checkpoint_ - 1].c_str())) {
            if (cps.empty() && ImGui::Selectable("Level start", true)) checkpoint_ = 0;
            for (int i = 0; i < static_cast<int>(cps.size()); ++i)
                if (ImGui::Selectable(cps[i].c_str(), checkpoint_ == i + 1)) checkpoint_ = i + 1;
            ImGui::EndCombo();
        }
    }

    // ---- the top of the panel ------------------------------------------------------------------------------------------
    // A row of buttons that work like radio buttons: the one that matches the state is drawn pressed.
    static bool ModeButton(const char* label, bool on, float width, ImU32 onColor)
    {
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, onColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, onColor);
        }
        const bool clicked = ImGui::Button(label, {width, 0});
        if (on) ImGui::PopStyleColor(2);
        return clicked && !on;
    }

    // One switch for the mode (Off / Record / Play), what's happening, a bar to see and move through the run, the
    // buttons that make sense right now, and the macro's name with Save and Load. Everything else is in the sections.
    void DrawControls(float width)
    {
        const tas::Status& st = status_;
        if (!st.available) {
            ImGui::TextWrapped("Unavailable: the engine hooks were not found (see the log).");
            return;
        }
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float third = (width - 2 * spacing) / 3.f;
        const bool idle = st.mode == tas::Mode::Idle;

        if (ModeButton("Off", idle, third, IM_COL32(90, 90, 90, 255))) tas::Stop();
        ImGui::SetItemTooltip("Stop recording / playing (%s)", keys::Name(stopKey).c_str());
        ImGui::SameLine();
        if (ModeButton("Record", st.mode == tas::Mode::Recording, third, IM_COL32(190, 50, 45, 255))) StartRecording();
        ImGui::SetItemTooltip("A new recording from the start set under Recording (%s with the menu closed)",
                              keys::Name(recordKey).c_str());
        ImGui::SameLine();
        if (ModeButton("Play", st.mode == tas::Mode::Playing, third, IM_COL32(45, 140, 70, 255))) tas::Play();
        ImGui::SetItemTooltip("Play the macro from its start. Your own input is ignored meanwhile (%s with the menu closed)",
                              keys::Name(playKey).c_str());

        DrawStatus();
        if (st.frames > 0 || !idle) DrawProgress(width);
        if (!idle) DrawTransport(width);
        if (st.frames > 0 || !idle) DrawSpeeds(width);
        DrawNameRow(width);
        if (idle && st.frames == 0) DrawFirstSteps(width);
        DrawGameplayWarning();
    }

    // The mode in its color, what the session last said, and how the last playback compared to the recording.
    void DrawStatus()
    {
        const tas::Status& st = status_;
        ImVec4 color = {0.75f, 0.75f, 0.75f, 1.f};
        if (st.mode == tas::Mode::Recording) color = {1.f, 0.42f, 0.38f, 1.f};
        if (st.mode == tas::Mode::Playing) color = {0.45f, 0.9f, 0.55f, 1.f};
        if (st.paused) color = {1.f, 0.8f, 0.35f, 1.f};

        ImGui::Spacing();
        ImGui::TextColored(color, "%s", StatusLine().c_str());
        if (st.desyncFrame >= 0) {
            ImGui::TextColored({1.f, 0.4f, 0.3f, 1.f}, "Desync at frame %lld", st.desyncFrame);
            if (!st.desyncInfo.empty()) ImGui::SetItemTooltip("%s", st.desyncInfo.c_str());
        } else if (st.corrections > 0) {
            ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Corrected %zu frame%s (not verified)", st.corrections,
                               st.corrections == 1 ? "" : "s");
            ImGui::SetItemTooltip("Small drift was put right: the player was moved back onto the recording");
        } else if (st.verified)
            ImGui::TextColored({0.4f, 1.f, 0.5f, 1.f}, "Verified: every frame matched");
        else if (!st.message.empty() && st.mode == tas::Mode::Idle)
            ImGui::TextDisabled("%s", st.message.c_str());
        ImGui::Spacing();
    }

    // How far into the run, with the bookmarks on it. Click (or drag) to go to a frame; idle, that plays up to it.
    void DrawProgress(float width)
    {
        const tas::Status& st = status_;
        const bool recording = st.mode == tas::Mode::Recording;
        const std::size_t total = std::max<std::size_t>(recording ? st.frame : st.frames, 1);
        const float fraction = recording ? 1.f : static_cast<float>(std::min(st.frame, total)) / static_cast<float>(total);
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "%zu / %zu", std::min(st.frame, total), total);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, recording ? IM_COL32(170, 55, 50, 255) : IM_COL32(60, 140, 80, 255));
        ImGui::ProgressBar(fraction, {width, 0}, overlay);
        ImGui::PopStyleColor();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();

        std::vector<tas::Marker> marks;
        tas::WithReplay([&](tas::Replay& r) { marks = r.markers; });
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const auto& m : marks) {
            const float x = a.x + (b.x - a.x) * static_cast<float>(m.frame) / static_cast<float>(total);
            if (x > b.x) continue;
            dl->AddTriangleFilled({x - 4.f, a.y}, {x + 4.f, a.y}, {x, a.y + 6.f}, IM_COL32(255, 210, 80, 255));
        }

        if (recording || !ImGui::IsItemHovered()) return;
        const float t = std::clamp((ImGui::GetIO().MousePos.x - a.x) / std::max(1.f, b.x - a.x), 0.f, 1.f);
        const auto frame = static_cast<std::size_t>(std::lround(t * static_cast<float>(total - 1)));
        const tas::Marker* mark = nullptr;     // The bookmark under the mouse
        for (const auto& m : marks)
            if (std::fabs(a.x + (b.x - a.x) * static_cast<float>(m.frame) / static_cast<float>(total) -
                          ImGui::GetIO().MousePos.x) < 5.f) mark = &m;
        if (mark) ImGui::SetTooltip("%s (frame %u): click to go there", mark->name.c_str(), mark->frame);
        else ImGui::SetTooltip("Frame %zu: click to go there", frame);
        if (ImGui::IsItemClicked()) tas::Goto(mark ? mark->frame : frame);
    }

    // Only while a session runs, and only the buttons that work in its mode.
    void DrawTransport(float width)
    {
        const tas::Status& st = status_;
        const float quarter = (width - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.f;
        if (ImGui::Button(st.paused ? "Resume" : "Pause", {quarter, 0})) tas::TogglePause();
        ImGui::SetItemTooltip("%s", keys::Name(pauseKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Step", {quarter, 0})) tas::Advance();
        ImGui::SetItemTooltip("Pause, then one frame per click (%s)", keys::Name(advanceKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Restart", {quarter, 0})) tas::Restart();
        ImGui::SetItemTooltip("%s (%s)", st.mode == tas::Mode::Playing ? "Play again from the start"
                                                                       : "Throw this take away and record again",
                              keys::Name(restartKey).c_str());
        ImGui::SameLine();
        if (st.mode == tas::Mode::Recording) {
            if (ImGui::Button("Rewind", {quarter, 0})) tas::Rewind();
            ImGui::SetItemTooltip("Go back %.1f s and keep recording from there (%s)", rewindSeconds.Get(),
                                  keys::Name(rewindKey).c_str());
        } else {
            if (ImGui::Button("Take over", {quarter, 0})) tas::TakeOver();
            ImGui::SetItemTooltip("Keep the playback up to here and record from this frame on (%s)",
                                  keys::Name(takeOverKey).c_str());
        }
    }

    // The speed of fixed-step frames, for playing and recording alike: slow down for a hard part, speed up through
    // the rest.
    void DrawSpeeds(float width)
    {
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float w = (width - spacing * (std::size(kSpeeds) - 1)) / static_cast<float>(std::size(kSpeeds));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing, ImGui::GetStyle().ItemSpacing.y});
        for (std::size_t i = 0; i < std::size(kSpeeds); ++i) {
            if (i) ImGui::SameLine();
            const float s = kSpeeds[i].speed;
            const bool on = s == 0.f ? maxSpeed.Get() : !maxSpeed.Get() && std::fabs(speed.Get() - s) < 1e-3f;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
            if (ImGui::Button(kSpeeds[i].label, {w, 0})) {
                maxSpeed.Set(s == 0.f);
                if (s > 0.f) speed.Set(s);
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();
    }

    // The macro's name, Save / Load under that name, and the timeline.
    void DrawNameRow(float width)
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float button = (width - 3 * spacing) * 0.2f;
        library_.DrawNameBox(width - 3 * (button + spacing));
        ImGui::SameLine();
        const bool unsaved = library_.Unsaved();
        if (unsaved) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.f, 0.82f, 0.4f, 1.f});
        if (ImGui::Button(unsaved ? "Save*" : "Save", {button, 0})) library_.Save();
        if (unsaved) ImGui::PopStyleColor();
        ImGui::SetItemTooltip(unsaved ? "Not saved yet: save it to replays\\ (a name is made up if the box is empty)"
                                      : "Save to replays\\ (a name is made up if the box is empty)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!library_.Name()[0]);
        if (ImGui::Button("Load", {button, 0})) {
            const std::filesystem::path path = tas::Folder() / (std::string(library_.Name()) + tas::kFileExtension);
            library_.Guard("Loading " + path.filename().string(), [this, path] { library_.Load(path); });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool open = showTimeline;
        if (open) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        const bool toggle = ImGui::Button("Edit", {button, 0});
        if (open) ImGui::PopStyleColor();
        if (toggle) showTimeline.Set(!open);
        ImGui::SetItemTooltip("%s the timeline: key holds, camera keyframes, bookmarks", showTimeline ? "Close" : "Open");
    }

    // With nothing loaded: how to get going.
    void DrawFirstSteps(float width)
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextDisabled("1. Pick where it starts under Recording.  2. Record (or close the menu and press %s).  "
                            "3. Stop with %s.  4. Play it back, or Save.",
                            keys::Name(recordKey).c_str(), keys::Name(stopKey).c_str());
        ImGui::PopTextWrapPos();
    }

    // Modules that change the game make a recording unverifiable (and its playback desync when they're off).
    template <class F>
    void ForEachGameplayMod(F&& f) const
    {
        modules::ForEach([&](modules::Tab& tab, Module& m) {
            if (!m.Enabled() || &m == this || m.IsAction()) return;
            if (tab.name != "Player" && tab.name != "World" && tab.name != "Trainer") return;
            if (m.Name() == "Tool Overlay" || m.Name() == "Speedometer" || m.Name() == "Speedrun Timer") return;
            f(m);
        });
    }

    std::string GameplayMods() const
    {
        std::string on;
        ForEachGameplayMod([&](Module& m) {
            if (!on.empty()) on += ", ";
            on += m.Name();
        });
        return on;
    }

    void DrawGameplayWarning()
    {
        const std::string on = GameplayMods();
        if (on.empty()) return;
        ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Changes the game: %s", on.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Turn off")) ForEachGameplayMod([](Module& m) { m.SetEnabled(false); });
        ImGui::SetItemTooltip("Recording or playing with these on makes the run play differently without them");
    }

    std::string StatusLine() const
    {
        const tas::Status& st = status_;
        char buf[128];
        switch (st.mode) {
            case tas::Mode::Idle:
                if (st.frames == 0) return "No macro loaded";
                tas::WithReplay([&](tas::Replay& r) {
                    std::snprintf(buf, sizeof buf, "Ready: %s, %zu frames", FormatTime(r.Duration()).c_str(),
                                  r.frames.size());
                });
                break;
            case tas::Mode::Recording:
                std::snprintf(buf, sizeof buf, "%s %zu  %s", st.armed ? "Loading..." : "REC", st.frame,
                              FormatTime(st.time).c_str());
                break;
            case tas::Mode::Playing:
                std::snprintf(buf, sizeof buf, "%s %zu / %zu  %s", st.armed ? "Loading..." : st.seeking ? "SEEK" : "PLAY",
                              st.frame, st.frames, FormatTime(st.time).c_str());
                break;
        }
        std::string s = buf;
        if (st.paused) s += "  (paused)";
        return s;
    }

    // ---- sections -------------------------------------------------------------------------------------------------------
    void DrawGoto(float width)
    {
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        ImGui::InputInt("##goto", &gotoFrame_, 0, 0);
        gotoFrame_ = std::max(gotoFrame_, 0);
        ImGui::SameLine();
        if (ImGui::Button("Go to frame", {half, 0})) tas::Goto(static_cast<std::size_t>(gotoFrame_));

        const float third = (width - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.f;
        if (ImGui::Button("|< Bookmark", {third, 0})) tas::GotoBookmark(-1);
        ImGui::SetItemTooltip("Go to the bookmark before this frame (%s)", keys::Name(prevBookmarkKey).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(status_.mode == tas::Mode::Idle);
        if (ImGui::Button("< Frame", {third, 0})) tas::FrameBack();
        ImGui::SetItemTooltip("One frame back: plays up to the frame before and pauses there. While recording, the take "
                              "is kept: take over there to redo that frame (%s)", keys::Name(frameBackKey).c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Bookmark >|", {third, 0})) tas::GotoBookmark(+1);
        ImGui::SetItemTooltip("Go to the next bookmark (%s)", keys::Name(nextBookmarkKey).c_str());
    }

    // Named frames: add one where the session is (or at the Go to frame), click to go there, rename, delete.
    void DrawBookmarks(float width)
    {
        const tas::Status& st = status_;
        std::vector<tas::Marker> marks;
        std::size_t frames = 0;
        tas::WithReplay([&](tas::Replay& r) {
            marks = r.markers;
            frames = r.frames.size();
        });
        const bool session = st.mode != tas::Mode::Idle;
        const std::size_t here = session ? st.frame : static_cast<std::size_t>(gotoFrame_);
        const bool canAdd = session || here < frames;
        char label[64];
        std::snprintf(label, sizeof label, "+ Bookmark frame %zu", here);
        ImGui::BeginDisabled(!canAdd);
        if (ImGui::Button(label, {width, 0}))
            tas::WithReplay([&](tas::Replay& r) {
                r.SetMarker(static_cast<std::uint32_t>(here), "Bookmark " + std::to_string(r.markers.size() + 1));
            });
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s (%s while a session runs)",
                              session ? "Where the session is now" : "The frame in Playback > Go to frame",
                              keys::Name(bookmarkKey).c_str());
        if (marks.empty()) {
            ImGui::TextDisabled("No bookmarks yet");
            return;
        }

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float frameW = ImGui::CalcTextSize("000000").x;
        const float goW = ImGui::GetFrameHeight() * 1.6f;
        for (const auto& m : marks) {
            ImGui::PushID(static_cast<int>(m.frame));
            char name[64];
            std::snprintf(name, sizeof name, "%s", m.name.c_str());
            ImGui::SetNextItemWidth(width - frameW - 2 * goW - 3 * spacing);
            if (ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivatedAfterEdit())
                tas::WithReplay([&](tas::Replay& r) { r.SetMarker(m.frame, name); });
            ImGui::SameLine();
            ImGui::TextDisabled("%6u", m.frame);
            ImGui::SameLine();
            if (ImGui::Button("Go", {goW, 0})) tas::Goto(m.frame);
            ImGui::SetItemTooltip("Play up to frame %u and pause there", m.frame);
            ImGui::SameLine();
            if (ImGui::Button("x", {goW, 0})) tas::WithReplay([&](tas::Replay& r) { r.EraseMarker(m.frame); });
            ImGui::SetItemTooltip("Delete this bookmark");
            ImGui::PopID();
        }
    }

    // What differs from when the macro was recorded, and what playback does about it. Refreshed twice a second.
    void DrawDesyncChecks(float width)
    {
        const double now = ImGui::GetTime();
        if (now - issuesTime_ > 0.5) {
            issues_ = tas::Preflight();
            issuesTime_ = now;
        }
        const tas::Status& st = status_;
        if (st.frames == 0) {
            ImGui::TextDisabled("No macro loaded");
        } else if (issues_.empty()) {
            ImGui::TextColored({0.4f, 1.f, 0.5f, 1.f}, "Same game and settings as when it was recorded");
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        bool modsDiffer = false;
        for (const tas::Issue& i : issues_) {
            const ImVec4 color = i.level == tas::Issue::Level::Problem ? ImVec4{1.f, 0.45f, 0.4f, 1.f}
                               : i.level == tas::Issue::Level::Warning ? ImVec4{1.f, 0.75f, 0.3f, 1.f}
                                                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            if (i.fixed) ImGui::TextColored({0.45f, 0.9f, 0.55f, 1.f}, "fixed on play:");
            else ImGui::TextColored(color, "%s", i.level == tas::Issue::Level::Info ? "-" : "!");
            ImGui::SameLine();
            ImGui::TextColored(i.fixed ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : color, "%s", i.text.c_str());
            modsDiffer |= i.text.rfind("Modules that change the game", 0) == 0;
        }
        ImGui::PopTextWrapPos();

        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(!modsDiffer || GameplayMods().empty());
        if (ImGui::Button("Turn them off", {half, 0})) ForEachGameplayMod([](Module& m) { m.SetEnabled(false); });
        ImGui::SetItemTooltip("Switch off the modules that change the game (God Mode, No Clip, Game Speed...)");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(st.frames == 0 || st.mode != tas::Mode::Idle);
        if (ImGui::Button(confirmResync_ ? "Sure?" : "Re-record sync", {half, 0})) {
            if (confirmResync_) tas::ResetSync();
            confirmResync_ = !confirmResync_;
        }
        if (!ImGui::IsItemHovered()) confirmResync_ = false;
        ImGui::SetItemTooltip("Forget the player states saved with the macro: the next playback saves them again and is "
                              "checked against from then on. For a game update, once it plays the way it should");
        ImGui::EndDisabled();
    }

    // The macro in memory: what it is, its ids, and the text saved with it.
    void DrawDetails(float width)
    {
        tas::WithReplay([&](tas::Replay& r) {
            const std::string fps = r.fixedHz > 0.f ? std::to_string(static_cast<int>(r.fixedHz)) + " fps" : "real time";
            const std::string from = r.start == tas::StartKind::Now       ? "from anywhere"
                                   : r.start == tas::StartKind::Command   ? r.command
                                   : r.map + (r.checkpoint.empty() ? "" : " @ " + r.checkpoint);
            ImGui::TextDisabled("%zu inputs, %u rerecords, %s, %s", r.events.size(), r.rerecords, fps.c_str(),
                                from.c_str());
            ImGui::TextDisabled("input %016llx  sync %016llx", static_cast<unsigned long long>(r.InputHash()),
                                static_cast<unsigned long long>(r.SyncHash()));
            ImGui::SetItemTooltip("Same input id = same start and inputs. The sync id is the recorded result");

            char author[64], desc[256];
            std::snprintf(author, sizeof author, "%s", r.author.c_str());
            std::snprintf(desc, sizeof desc, "%s", r.description.c_str());
            ImGui::SetNextItemWidth(width);
            if (ImGui::InputTextWithHint("##author", "author", author, sizeof author)) r.author = author;
            ImGui::SetNextItemWidth(width);
            if (ImGui::InputTextWithHint("##desc", "description", desc, sizeof desc)) r.description = desc;
        });
    }

    void DrawTranslator(float width)
    {
        ImGui::TextDisabled("Key translator");
        const float third = (width - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.f;
        ImGui::SetNextItemWidth(third);
        ImGui::InputTextWithHint("##from", "from", fromBuf_, sizeof fromBuf_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(third);
        ImGui::InputTextWithHint("##to", "to", toBuf_, sizeof toBuf_);
        ImGui::SameLine();
        if (ImGui::Button("Replace", {third, 0})) {
            std::size_t n = 0;
            tas::WithReplay([&](tas::Replay& r) { n = r.RenameKey(fromBuf_, toBuf_); });
            notify::Push("Replaced " + std::to_string(n) + " inputs");
        }
        ImGui::SetItemTooltip("Rewrites the replay for other key bindings, e.g. LeftMouseButton -> RightMouseButton");
    }

    // ---- splits -------------------------------------------------------------------------------------------------------------
    // The current macro's split table (refreshed a few times a second: it walks every frame).
    const tas::splits::Table& Splits()
    {
        const double now = ImGui::GetTime();
        if (now - splitsTime_ > 0.25) {
            splitsTime_ = now;
            tas::WithReplay([&](tas::Replay& r) {
                splits_ = tas::splits::Build(r, hasReference_ ? &reference_ : nullptr, splitsOnly);
            });
        }
        return splits_;
    }

    void SetReference(tas::Replay r, std::string name)
    {
        reference_ = std::move(r);
        referenceName_ = std::move(name);
        hasReference_ = true;
        splitsTime_ = -1.0;
    }

    void DrawReferencePicker(float width)
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float button = (width - spacing) * 0.35f;
        ImGui::SetNextItemWidth(width - button - spacing);
        const std::string preview = hasReference_ ? referenceName_ : std::string("Compare with...");
        if (ImGui::BeginCombo("##reference", preview.c_str())) {
            if (ImGui::Selectable("Nothing", !hasReference_)) {
                hasReference_ = false;
                referenceName_.clear();
                splitsTime_ = -1.0;
            }
            namespace fs = std::filesystem;
            std::error_code ec;
            std::vector<fs::path> found;
            for (const auto& e : fs::directory_iterator(tas::Folder(), ec))
                if (e.is_regular_file(ec) && (e.path().extension() == tas::kFileExtension || e.path().extension() == ".json"))
                    found.push_back(e.path());
            std::sort(found.begin(), found.end());
            for (const fs::path& f : found) {
                const std::string name = f.stem().string();
                if (!ImGui::Selectable(name.c_str(), hasReference_ && name == referenceName_)) continue;
                tas::Replay r;
                std::string error;
                const bool ok = f.extension() == ".json" ? r.ImportJson(f, &error) : r.Load(f, &error);
                if (ok) SetReference(std::move(r), name);
                else notify::Push("TAS: can't compare with " + name + ": " + error);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("A saved macro (replays\\) to compare the splits with. Splits are matched by name");
        ImGui::SameLine();
        ImGui::BeginDisabled(status_.frames == 0);
        if (ImGui::Button("This one", {button, 0})) {
            tas::Replay r;
            tas::WithReplay([&](tas::Replay& cur) { r = cur; });
            const std::string name = "snapshot " + FormatTime(r.Duration());
            SetReference(std::move(r), name);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Compare with the macro as it is now: optimise it, then see what each split gained");
    }

    // Split | time | segment | vs the reference, for every split (or bookmark), then the end of the run.
    void DrawSplitTable(float width)
    {
        DrawReferencePicker(width);
        const tas::splits::Table& t = Splits();
        if (t.rows.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextDisabled("%s", splitsOnly.Get()
                                          ? "No splits yet: record or play a run that completes a level (with automatic "
                                            "splits on), or turn off Only automatic splits to use your bookmarks"
                                          : "No bookmarks yet");
            ImGui::PopTextWrapPos();
        }
        const bool ref = hasReference_;
        const int current = status_.mode == tas::Mode::Idle ? -1 : t.RowAt(status_.frame);
        if (ImGui::BeginTable("##splits", ref ? 4 : 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg,
                              {width, 0})) {
            ImGui::TableSetupColumn("Split", ImGuiTableColumnFlags_WidthStretch, 2.f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Segment", ImGuiTableColumnFlags_WidthStretch, 1.f);
            if (ref) ImGui::TableSetupColumn("+/-", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < t.rows.size(); ++i) {
                const tas::splits::Row& row = t.rows[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                if (static_cast<int>(i) == current)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.5f));
                ImGui::TableNextColumn();
                if (ImGui::Selectable(SplitLabel(row.name), false, ImGuiSelectableFlags_SpanAllColumns)) tas::Goto(row.frame);
                ImGui::SetItemTooltip("Frame %u: click to go there", row.frame);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatTime(row.time).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatTime(row.segment).c_str());
                if (ref) {
                    ImGui::TableNextColumn();
                    if (!row.HasRef()) {
                        ImGui::TextDisabled("-");
                    } else {
                        ImGui::TextColored(DeltaColor(row.Delta()), "%s", FormatDelta(row.Delta()).c_str());
                        ImGui::SetItemTooltip("Reference: %s\nThis segment: %s (reference %s)",
                                              FormatTime(row.refTime).c_str(), FormatDelta(row.SegmentDelta()).c_str(),
                                              FormatTime(row.refSegment).c_str());
                    }
                }
                ImGui::PopID();
            }
            // The whole run
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("End");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatTime(t.end).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatTime(t.end - (t.rows.empty() ? 0.0 : t.rows.back().time)).c_str());
            if (ref) {
                ImGui::TableNextColumn();
                ImGui::TextColored(DeltaColor(t.end - t.refEnd), "%s", FormatDelta(t.end - t.refEnd).c_str());
                ImGui::SetItemTooltip("Reference: %s", FormatTime(t.refEnd).c_str());
            }
            ImGui::EndTable();
        }
        if (ref) {
            const double sob = t.SumOfBest();
            if (sob >= 0.0)
                ImGui::TextDisabled("Best of both, segment by segment: %s (to the last split)", FormatTime(sob).c_str());
        }
    }

    // ---- overlay ----------------------------------------------------------------------------------------------------------
    void DrawOverlay()
    {
        const tas::Status& st = status_;
        if (st.mode == tas::Mode::Idle && st.desyncFrame < 0 && !st.verified) return;
        std::string text = StatusLine();
        ImU32 color = IM_COL32(255, 255, 255, 255);
        if (st.mode == tas::Mode::Recording) color = IM_COL32(255, 90, 80, 255);
        if (st.desyncFrame >= 0) text += "  desync @" + std::to_string(st.desyncFrame);
        else if (st.verified) text += "  verified";
        // The last split passed, against the reference
        std::string deltaText;
        double delta = 0.0;
        if (hasReference_ && st.mode != tas::Mode::Idle) {
            const tas::splits::Table& t = Splits();
            if (const int row = t.RowAt(st.frame); row >= 0 && t.rows[row].HasRef()) {
                delta = t.rows[row].Delta();
                deltaText = " " + FormatDelta(delta);
                text += std::string("  ") + SplitLabel(t.rows[row].name);
            }
        }
        if (st.paused) text += "   [" + keys::Name(advanceKey) + "] step  [" + keys::Name(pauseKey) + "] resume";
        if (!maxSpeed && std::fabs(speed.Get() - 1.f) > 1e-3f && st.mode != tas::Mode::Idle) {
            char s[24];
            std::snprintf(s, sizeof s, "  %.2gx", speed.Get());
            text += s;
        }

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const float dw = deltaText.empty() ? 0.f : ImGui::CalcTextSize(deltaText.c_str()).x;
        dl->AddRectFilled({8.f, 8.f}, {16.f + ts.x + dw, 12.f + ts.y}, IM_COL32(0, 0, 0, 150));
        dl->AddText({12.f, 10.f}, color, text.c_str());
        if (!deltaText.empty())
            dl->AddText({12.f + ts.x, 10.f}, ImGui::ColorConvertFloat4ToU32(DeltaColor(delta)), deltaText.c_str());
    }

    // ---- state ------------------------------------------------------------------------------------------------------------
    tas::Status status_;
    tasui::TimelineView timeline_;
    tasui::ReplayEditor editor_;
    tasui::MacroLibrary library_;
    std::vector<tas::Issue> issues_;
    double issuesTime_ = -1.0;
    bool confirmResync_ = false;
    char commandBuf_[128] = "open edge_p";
    char fromBuf_[48] = "", toBuf_[48] = "";
    int map_ = 0, checkpoint_ = 1;     // Checkpoint A (the level start)
    int gotoFrame_ = 0;
    std::vector<int> reserved_;
    tas::Replay reference_;               // What the splits are compared with
    bool hasReference_ = false;
    std::string referenceName_;
    tas::splits::Table splits_;
    double splitsTime_ = -1.0;

    // ---- controls (drawn in this order) -----------------------------------------------------------------------------------------
    ctl::Custom controls{this, [this](float w) { DrawControls(w); }};

    ctl::Section recordSection{this, "Recording", true};
    ctl::Combo start{this, "Start", {"Now (macro)", "Console command", "Level / checkpoint"}, 2,
                     "Now: from where you are (can't be verified). A level start makes the run reproducible"};
    ctl::Custom command{this, [this](float w) { DrawCommand(w); }};
    ctl::Custom level{this, [this](float w) { DrawLevel(w); }};
    ctl::Combo rate{this, "Frame rate", {"Real time", "30", "50", "60", "62", "100", "120", "144", "240"}, 3,
                    "Fixed: every frame is the same length (TAS). Real time: each frame's length is stored (speedruns)"};
    ctl::Slider realtimeCap{this, "Frame cap", 62.f, 0.f, 300.f, "%.0f fps", 1.f, "0 = uncapped. The game's own cap is 62"};
    ctl::Combo onDeath{this, "On death", {"Keep recording", "Restart attempt", "Rewind (practice)"}, 2,
                       "Practice: when you die, go back a little and keep recording from there, so the macro only has "
                       "the good attempt. Restart: start the recording over from the start level"};
    ctl::Slider rewindSeconds{this, "Rewind by", 2.f, 0.25f, 10.f, "%.2f s", 0.25f,
                              "How far back practice mode (and the Rewind key) goes"};

    ctl::Section playSection{this, "Playback", false};
    ctl::Slider speed{this, "Speed", 1.f, 0.05f, 8.f, "%.2fx", 0.05f,
                      "Of fixed-step frames, playing and recording (the buttons under the progress bar set it too)"};
    ctl::Checkbox maxSpeed{this, "As fast as possible", false};
    ctl::Combo atEnd{this, "At the end", {"Stop", "Pause", "Keep recording", "Loop"}, 1,
                     "Keep recording: carry on from the last frame. Loop: play it again from the start"};
    ctl::Custom gotoFrame{this, [this](float w) { DrawGoto(w); }};

    ctl::Section bookmarkSection{this, "Bookmarks", false};
    ctl::Checkbox autoSplits{this, "Automatic splits", true,
                             "Recording and playing: a bookmark \"Split: <level>\" on the frame each level is completed. "
                             "A playback of an edited run moves them to where it completes the level now"};
    ctl::Custom bookmarks{this, [this](float w) { DrawBookmarks(w); }};

    ctl::Section splitsSection{this, "Splits", false};
    ctl::Checkbox splitsOnly{this, "Only automatic splits", true,
                             "Off: every bookmark is a split (for your own segments inside a level)"};
    ctl::Custom splitTable{this, [this](float w) { DrawSplitTable(w); }};

    ctl::Section desyncSection{this, "Desync", false};
    ctl::Custom desyncChecks{this, [this](float w) { DrawDesyncChecks(w); }};
    ctl::Combo onDesync{this, "On desync", {"Pause", "Correct small drift", "Keep going"}, 0,
                        "When a playback stops matching the recording. Pause: stop there (take over to record a fix). "
                        "Correct: put the player back on the recording if it's a small difference in the same move, "
                        "and go on (not verified then). Keep going: note it and go on"};
    ctl::Slider correctMax{this, "Correct up to", 64.f, 1.f, 512.f, "%.0f units", 8.f,
                           "The furthest a correction moves the player. Further off is a real desync: it pauses"};
    ctl::Checkbox matchRecording{this, "Match the recording", true,
                                 "On play, use the recording's streaming setting and mouse sensitivity, and play keys "
                                 "whose binding changed as the key that does that now. Your settings come back after"};

    ctl::Section filesSection{this, "Macros", true};
    ctl::Custom files{this, [this](float w) { library_.DrawList(w); }};

    ctl::Section keysSection{this, "Keys", false};
    ctl::KeyHold recordKey{this, "Record", VK_F2};
    ctl::KeyHold playKey{this, "Play", VK_F3};
    ctl::KeyHold pauseKey{this, "Pause / resume", VK_F5};
    ctl::KeyHold advanceKey{this, "Frame advance", VK_F6};
    ctl::KeyHold frameBackKey{this, "Frame back", 0, "Replays up to the frame before this one and pauses there"};
    ctl::KeyHold restartKey{this, "Restart", VK_F4};
    ctl::KeyHold rewindKey{this, "Rewind", VK_F9};
    ctl::KeyHold takeOverKey{this, "Take over", VK_F7};
    ctl::KeyHold stopKey{this, "Stop", VK_F8};
    ctl::KeyHold bookmarkKey{this, "Bookmark", VK_F1};
    ctl::KeyHold prevBookmarkKey{this, "Previous bookmark", 0};
    ctl::KeyHold nextBookmarkKey{this, "Next bookmark", 0, "Idle with the menu closed: plays to the first bookmark"};

    ctl::Section moreSection{this, "Advanced", false};
    ctl::Checkbox blockStreaming{this, "Deterministic streaming", true,
                                 "Load streamed level parts in the frame they are requested (a short hitch, but every run "
                                 "and every PC gets them on the same frame). Recordings need the same setting to replay"};
    ctl::Checkbox seekNoRender{this, "Don't render while seeking", false,
                               "Faster Go to frame. Animations of unseen things may update differently: can desync"};
    ctl::Checkbox showOverlay{this, "Status overlay", true};
    ctl::Checkbox showTimeline{this, "Timeline", false, "Keyframe editor: key holds as bars, camera keyframes"};
    ctl::Checkbox showEditor{this, "Replay editor", false,
                             "Frame by frame: step the game back and forth, go to any frame, see and set the keys "
                             "held on each frame"};
    ctl::Checkbox showInputDisplay{this, "Input display", true,
                                   "While a macro runs: its keys at the bottom left of the screen, lit while held"};
    ctl::Custom details{this, [this](float w) { DrawDetails(w); }};
    ctl::Custom translator{this, [this](float w) { DrawTranslator(w); }};
};
MEM_MODULE(Tas)

} // namespace
