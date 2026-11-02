#include "core/module.h"
#include "core/notify.h"
#include "game/levels.h"
#include "gui.h"
#include "features/tas/replay_editor.h"
#include "features/tas/timeline_view.h"
#include "tas/tas.h"

#include <Windows.h>
#include <imgui.h>
#include <imfilebrowser.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
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

const float kRates[] = {0.f, 30.f, 50.f, 60.f, 62.f, 100.f, 120.f, 144.f, 240.f};

bool IsMacroFile(const std::filesystem::path& path)
{
    const auto ext = path.extension();
    return ext == tas::kFileExtension || ext == ".json";
}

bool ContainsNoCase(const std::string& text, const std::string& part)
{
    if (part.empty()) return true;
    auto it = std::search(text.begin(), text.end(), part.begin(), part.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                      std::tolower(static_cast<unsigned char>(b)); });
    return it != text.end();
}

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
        in(desyncSection, {&desyncChecks, &onDesync, &matchRecording});
        correctMax.VisibleIf([this] { return desyncSection && onDesync.Get() == 1; });
        speed.VisibleIf([this] { return playSection && !maxSpeed.Get(); });

        in(filesSection, {&files});
        in(keysSection, {&pauseKey, &advanceKey, &restartKey, &rewindKey, &takeOverKey, &stopKey});
        in(moreSection, {&blockStreaming, &seekNoRender, &showOverlay, &showInputDisplay, &showTimeline, &showEditor, &details, &translator});
    }

    void OnDisable() override
    {
        tas::Stop();
        if (openBrowser_) openBrowser_->Close();
        if (saveBrowser_) saveBrowser_->Close();
    }

    void OnRender() override
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
        o.keyPause = pauseKey;
        o.keyAdvance = advanceKey;
        o.keyTakeOver = takeOverKey;
        o.keyStop = stopKey;
        o.keyRewind = rewindKey;
        o.keyRestart = restartKey;
        o.onDeath = static_cast<tas::OnDeath>(onDeath.Get());
        o.rewindSeconds = rewindSeconds;
        tas::SetOptions(o);

        status_ = tas::GetStatus();
        if (showOverlay) DrawOverlay();
        if (showEditor) {
            bool open = true;
            editor_.Draw(open, status_, {KeyLabel(pauseKey), KeyLabel(advanceKey)});
            if (!open) showEditor.Set(false);
        }
        if (showInputDisplay) tasui::ReplayEditor::DrawInputDisplay(status_);
        if (showTimeline) {
            bool open = true;
            timeline_.Draw(open, status_);
            if (!open) showTimeline.Set(false);
        }
        if (gui::visible) DrawBrowsers();
    }

private:
    // ---- panel ----------------------------------------------------------------------------------------------------------
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
                if (ImGui::Selectable(maps[i].label.c_str(), i == map_)) { map_ = i; checkpoint_ = 0; }
            ImGui::EndCombo();
        }
        const auto& cps = maps[map_].checkpoints;
        checkpoint_ = std::clamp(checkpoint_, 0, static_cast<int>(cps.size()));
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##cp", checkpoint_ == 0 ? "Level start" : cps[checkpoint_ - 1].c_str())) {
            if (ImGui::Selectable("Level start", checkpoint_ == 0)) checkpoint_ = 0;
            for (int i = 0; i < static_cast<int>(cps.size()); ++i)
                if (ImGui::Selectable(cps[i].c_str(), checkpoint_ == i + 1)) checkpoint_ = i + 1;
            ImGui::EndCombo();
        }
    }

    void StartRecording()
    {
        std::string mapLabel, cpLabel;
        if (start.Get() == 2) {
            const auto maps = levels::Maps();
            if (map_ < 0 || map_ >= static_cast<int>(maps.size())) { notify::Push("TAS: pick a level"); return; }
            mapLabel = maps[map_].label;
            if (checkpoint_ > 0 && checkpoint_ <= static_cast<int>(maps[map_].checkpoints.size()))
                cpLabel = maps[map_].checkpoints[checkpoint_ - 1];
        }
        tas::Record(static_cast<tas::StartKind>(start.Get()), commandBuf_, mapLabel, cpLabel);
    }

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

    // Like Mega Hack's replay bot: one switch for the mode (Off / Record / Play), what's happening, the buttons that
    // make sense right now, and the macro's name with Save and Load. Everything else is in the sections below.
    void DrawControls(float width)
    {
        const tas::Status& st = status_;
        if (!st.available) {
            ImGui::TextWrapped("Unavailable: the engine hooks were not found (see the log).");
            return;
        }
        const float third = (width - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.f;
        const bool idle = st.mode == tas::Mode::Idle;

        if (ModeButton("Off", idle, third, IM_COL32(90, 90, 90, 255))) tas::Stop();
        ImGui::SetItemTooltip("Stop recording / playing (%s)", KeyLabel(stopKey).c_str());
        ImGui::SameLine();
        if (ModeButton("Record", st.mode == tas::Mode::Recording, third, IM_COL32(190, 50, 45, 255))) StartRecording();
        ImGui::SetItemTooltip("A new recording from the start set under Recording. Replaces the macro in memory");
        ImGui::SameLine();
        if (ModeButton("Play", st.mode == tas::Mode::Playing, third, IM_COL32(45, 140, 70, 255))) tas::Play();
        ImGui::SetItemTooltip("Play the macro from its start. Your own input is ignored meanwhile");

        DrawStatus();
        if (!idle) DrawTransport(width);
        DrawMacroName(width);
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

    // Only while a session runs, and only the buttons that work in its mode.
    void DrawTransport(float width)
    {
        const tas::Status& st = status_;
        const float quarter = (width - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.f;
        if (ImGui::Button(st.paused ? "Resume" : "Pause", {quarter, 0})) tas::TogglePause();
        ImGui::SetItemTooltip("%s", KeyLabel(pauseKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Step", {quarter, 0})) tas::Advance();
        ImGui::SetItemTooltip("Pause, then one frame per click (%s)", KeyLabel(advanceKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Restart", {quarter, 0})) tas::Restart();
        ImGui::SetItemTooltip("%s (%s)", st.mode == tas::Mode::Playing ? "Play again from the start"
                                                                       : "Throw this take away and record again",
                              KeyLabel(restartKey).c_str());
        ImGui::SameLine();
        if (st.mode == tas::Mode::Recording) {
            if (ImGui::Button("Rewind", {quarter, 0})) tas::Rewind();
            ImGui::SetItemTooltip("Go back %.1f s and keep recording from there (%s)", rewindSeconds.Get(),
                                  KeyLabel(rewindKey).c_str());
        } else {
            if (ImGui::Button("Take over", {quarter, 0})) tas::TakeOver();
            ImGui::SetItemTooltip("Keep the playback up to here and record from this frame on (%s)",
                                  KeyLabel(takeOverKey).c_str());
        }
    }

    void DrawGoto(float width)
    {
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        ImGui::InputInt("##goto", &gotoFrame_, 0, 0);
        gotoFrame_ = std::max(gotoFrame_, 0);
        ImGui::SameLine();
        if (ImGui::Button("Go to frame", {half, 0})) tas::Goto(static_cast<std::size_t>(gotoFrame_));
    }

    // The macro's name, and Save / Load into the macros folder under that name (Mega Hack's layout).
    void DrawMacroName(float width)
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float button = (width - 3 * spacing) * 0.2f;
        ImGui::SetNextItemWidth(width - 3 * (button + spacing));
        ImGui::InputTextWithHint("##file", "macro name", fileBuf_, sizeof fileBuf_);
        const bool named = fileBuf_[0] != 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(!named);
        if (ImGui::Button("Save", {button, 0})) SaveNamed();
        ImGui::SetItemTooltip("Save to replays/%s%s (next to the menu DLL)", fileBuf_, tas::kFileExtension);
        ImGui::SameLine();
        if (ImGui::Button("Load", {button, 0})) LoadPath(NamedPath());
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool open = showTimeline;
        if (open) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        const bool toggle = ImGui::Button("Edit", {button, 0});
        if (open) ImGui::PopStyleColor();
        if (toggle) showTimeline.Set(!open);
        ImGui::SetItemTooltip("%s the timeline: key holds, camera keyframes", showTimeline ? "Close" : "Open");
    }
    // Modules that change the game make a recording unverifiable (and its playback desync when they're off).
    template <class F>
    void ForEachGameplayMod(F&& f)
    {
        modules::ForEach([&](modules::Tab& tab, Module& m) {
            if (!m.Enabled() || &m == this || m.IsAction()) return;
            if (tab.name != "Player" && tab.name != "World" && tab.name != "Trainer") return;
            if (m.Name() == "Tool Overlay" || m.Name() == "Speedometer") return;
            f(m);
        });
    }

    std::string GameplayMods()
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
        if (!on.empty()) ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Changes the game: %s", on.c_str());
    }

    // ---- desync ---------------------------------------------------------------------------------------------------------------
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

    struct MacroFile {
        std::filesystem::path path;
        std::string label;                 // file name: "run.merp", "showcase.json"
        std::string name;                  // without the extension
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time;
    };

    void RefreshFiles()
    {
        files_.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(tas::Folder(), ec)) {
            if (!e.is_regular_file(ec) || !IsMacroFile(e.path())) continue;
            MacroFile f;
            f.path = e.path();
            f.label = e.path().filename().string();
            f.name = e.path().stem().string();
            f.size = e.file_size(ec);
            f.time = e.last_write_time(ec);
            files_.push_back(std::move(f));
        }
        // Newest first, like a recent list
        std::sort(files_.begin(), files_.end(), [](const MacroFile& a, const MacroFile& b) { return a.time > b.time; });
        filesLoaded_ = true;
    }

    // Where Save writes the name in the box: replays/<name>.merp
    std::filesystem::path FilePath() const { return tas::Folder() / (std::string(fileBuf_) + tas::kFileExtension); }

    // The file Load / Delete mean: the one picked in the list, else <name>.merp, else <name>.json.
    std::filesystem::path NamedPath() const
    {
        if (!selectedFile_.empty() && selectedFile_.stem().string() == fileBuf_) return selectedFile_;
        std::error_code ec;
        const auto merp = FilePath();
        if (std::filesystem::exists(merp, ec)) return merp;
        const auto json = tas::Folder() / (std::string(fileBuf_) + ".json");
        return std::filesystem::exists(json, ec) ? json : merp;
    }

    void SaveTo(const std::filesystem::path& path)
    {
        std::string error;
        tas::WithReplay([&](tas::Replay& r) {
            if (r.description.empty()) r.description = path.stem().string();
        });
        if (tas::SaveFile(path, &error)) {
            notify::Push("Saved " + path.filename().string());
            std::snprintf(fileBuf_, sizeof fileBuf_, "%s", path.stem().string().c_str());
            selectedFile_ = path;
        } else {
            notify::Push("TAS: " + error);
        }
        RefreshFiles();
    }

    void SaveNamed() { SaveTo(FilePath()); }

    void LoadPath(const std::filesystem::path& path)
    {
        std::string error;
        if (tas::LoadFile(path, &error)) {
            notify::Push("Loaded " + path.filename().string());
            std::snprintf(fileBuf_, sizeof fileBuf_, "%s", path.stem().string().c_str());
            selectedFile_ = path;
        } else {
            notify::Push("TAS: " + error);
        }
    }

    // ---- file browser (imgui-filebrowser: drawn by the menu, so it is never behind the game or stuck) ------------------
    static constexpr ImGuiFileBrowserFlags kBrowserFlags =
        ImGuiFileBrowserFlags_CloseOnEsc | ImGuiFileBrowserFlags_ConfirmOnEnter |
        ImGuiFileBrowserFlags_SkipItemsCausingError | ImGuiFileBrowserFlags_EditPathString;

    // Created the first time they are opened: the constructor reads the disk (folders, drive letters).
    ImGui::FileBrowser& Browser(bool save)
    {
        auto& browser = save ? saveBrowser_ : openBrowser_;
        if (!browser) {
            std::error_code ec;
            std::filesystem::create_directories(tas::Folder(), ec);
            browser.emplace(save ? kBrowserFlags | ImGuiFileBrowserFlags_EnterNewFilename |
                                       ImGuiFileBrowserFlags_CreateNewDir
                                 : kBrowserFlags,
                            tas::Folder());
            browser->SetWindowSize(720, 460);
        }
        return *browser;
    }

    void OpenBrowser(bool save)
    {
        ImGui::FileBrowser& b = Browser(save);
        b.SetTypeFilters({".merp", ".json"});
        if (save) {
            b.SetTitle("Save macro as");
            b.SetInputName(std::string(fileBuf_[0] ? fileBuf_ : "macro") + tas::kFileExtension);
        } else {
            b.SetTitle("Open macro");
        }
        b.Open();
    }

    void DrawBrowsers()
    {
        if (openBrowser_) {
            openBrowser_->Display();
            if (openBrowser_->HasSelected()) {
                LoadPath(openBrowser_->GetSelected());
                openBrowser_->ClearSelected();
            }
        }
        if (saveBrowser_) {
            saveBrowser_->Display();
            if (saveBrowser_->HasSelected()) {
                std::filesystem::path path = saveBrowser_->GetSelected();
                saveBrowser_->ClearSelected();
                if (!IsMacroFile(path)) path += tas::kFileExtension;     // .json only when asked for by name
                std::error_code ec;
                if (std::filesystem::exists(path, ec)) {
                    overwrite_ = path;
                    ImGui::OpenPopup("Replace file?##tas");
                } else {
                    SaveTo(path);
                }
            }
        }

        if (ImGui::BeginPopupModal("Replace file?##tas", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s already exists. Replace it?", overwrite_.filename().string().c_str());
            ImGui::TextDisabled("%s", overwrite_.parent_path().string().c_str());
            if (ImGui::Button("Replace", {120.f, 0})) {
                SaveTo(overwrite_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120.f, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void DrawFiles(float width)
    {
        // The folder is small: looking again every couple of seconds keeps the list current without a Refresh button.
        const double now = ImGui::GetTime();
        if (!filesLoaded_ || now - filesTime_ > 2.0) {
            RefreshFiles();
            filesTime_ = now;
        }
        const float quarter = (width - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.f;

        // The macros folder, newest first
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##search", "search", searchBuf_, sizeof searchBuf_);
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginListBox("##macros", {width, rowH * 5.f + 4.f})) {
            int shown = 0;
            for (const auto& f : files_) {
                if (!ContainsNoCase(f.label, searchBuf_)) continue;
                ++shown;
                const bool selected = f.path == selectedFile_;
                if (ImGui::Selectable(f.label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    std::snprintf(fileBuf_, sizeof fileBuf_, "%s", f.name.c_str());
                    selectedFile_ = f.path;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) LoadPath(f.path);
                }
                ImGui::SetItemTooltip("Double click to load (%.1f KB)", static_cast<double>(f.size) / 1024.0);
            }
            if (shown == 0) ImGui::TextDisabled(files_.empty() ? "No saved macros yet" : "No match");
            ImGui::EndListBox();
        }

        // Any file, anywhere: the file browser
        if (ImGui::Button("Open...", {quarter, 0})) OpenBrowser(false);
        ImGui::SetItemTooltip("Open a .merp or .json macro from anywhere");
        ImGui::SameLine();
        if (ImGui::Button("Save as...", {quarter, 0})) OpenBrowser(true);
        ImGui::SetItemTooltip("Save anywhere: .merp, or .json for readable, editable text");
        ImGui::SameLine();

        const bool named = fileBuf_[0] != 0;
        ImGui::BeginDisabled(!named);
        if (ImGui::Button(confirmDelete_ ? "Sure?" : "Delete", {quarter, 0})) {
            if (!confirmDelete_) {
                confirmDelete_ = true;
            } else {
                confirmDelete_ = false;
                const auto path = NamedPath();
                std::error_code ec;
                if (std::filesystem::remove(path, ec)) notify::Push("Deleted " + path.filename().string());
                else notify::Push("TAS: couldn't delete " + path.filename().string());
                if (path == selectedFile_) selectedFile_.clear();
                RefreshFiles();
            }
        }
        if (!ImGui::IsItemHovered()) confirmDelete_ = false;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("New", {quarter, 0})) {
            tas::Clear();
            fileBuf_[0] = 0;
            selectedFile_.clear();
        }
        ImGui::SetItemTooltip("Stop and empty the macro in memory (saved files are kept)");
    }

    // The macro in memory: what it is, its ids, and the text saved with it.
    void DrawDetails(float width)
    {
        tas::WithReplay([&](tas::Replay& r) {
            const std::string rate = r.fixedHz > 0.f ? std::to_string(static_cast<int>(r.fixedHz)) + " fps" : "real time";
            const std::string from = r.start == tas::StartKind::Now       ? "from anywhere"
                                   : r.start == tas::StartKind::Command   ? r.command
                                   : r.map + (r.checkpoint.empty() ? "" : " @ " + r.checkpoint);
            ImGui::TextDisabled("%zu inputs, %u rerecords, %s, %s", r.events.size(), r.rerecords, rate.c_str(),
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
        if (st.paused) text += "   [" + KeyLabel(advanceKey) + "] step  [" + KeyLabel(pauseKey) + "] resume";

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled({8.f, 8.f}, {16.f + ts.x, 12.f + ts.y}, IM_COL32(0, 0, 0, 150));
        dl->AddText({12.f, 10.f}, color, text.c_str());
    }

    static std::string KeyLabel(int vk)
    {
        char name[32] = "?";
        if (const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC))
            GetKeyNameTextA(static_cast<LONG>(sc << 16), name, sizeof name);
        return name;
    }

    // ---- state ------------------------------------------------------------------------------------------------------------
    tas::Status status_;
    tasui::TimelineView timeline_;
    tasui::ReplayEditor editor_;
    std::vector<tas::Issue> issues_;
    double issuesTime_ = -1.0;
    bool confirmResync_ = false;
    char commandBuf_[128] = "open edge_p";
    char fileBuf_[96] = "";
    char fromBuf_[48] = "", toBuf_[48] = "";
    int map_ = 0, checkpoint_ = 0;
    int gotoFrame_ = 0;
    std::vector<MacroFile> files_;
    bool filesLoaded_ = false;
    double filesTime_ = 0.0;
    char searchBuf_[64] = "";
    bool confirmDelete_ = false;
    std::filesystem::path selectedFile_;           // picked in the macro list (or last loaded / saved)
    std::optional<ImGui::FileBrowser> openBrowser_, saveBrowser_;
    std::filesystem::path overwrite_;              // "Replace file?" asks about this one

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
    ctl::Slider speed{this, "Speed", 1.f, 0.05f, 8.f, "%.2fx", 0.05f};
    ctl::Checkbox maxSpeed{this, "As fast as possible", false};
    ctl::Combo atEnd{this, "At the end", {"Stop", "Pause", "Keep recording"}, 1};

    ctl::Custom gotoFrame{this, [this](float w) { DrawGoto(w); }};

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
    ctl::Custom files{this, [this](float w) { DrawFiles(w); }};

    ctl::Section keysSection{this, "Keys", false};
    ctl::KeyHold pauseKey{this, "Pause / resume", VK_F5};
    ctl::KeyHold advanceKey{this, "Frame advance", VK_F6};
    ctl::KeyHold restartKey{this, "Restart", VK_F4};
    ctl::KeyHold rewindKey{this, "Rewind", VK_F9};
    ctl::KeyHold takeOverKey{this, "Take over", VK_F7};
    ctl::KeyHold stopKey{this, "Stop", VK_F8};

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
