#include "core/module.h"
#include "core/notify.h"
#include "game/levels.h"
#include "tas/tas.h"

#include <Windows.h>
#include <commdlg.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comdlg32.lib")

namespace {

std::string FormatTime(double seconds)
{
    char buf[32];
    const int m = static_cast<int>(seconds / 60.0);
    std::snprintf(buf, sizeof buf, "%d:%06.3f", m, seconds - m * 60.0);
    return buf;
}

const float kRates[] = {0.f, 30.f, 50.f, 60.f, 62.f, 100.f, 120.f, 144.f, 240.f};

// The game's main window: owner of the file dialogs, so they open in front of it.
HWND GameWindow()
{
    struct Find { HWND found = nullptr; } f;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        reinterpret_cast<Find*>(lp)->found = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// Windows' Open / Save As dialog, on its own thread: it is modal and would freeze the menu (and with it the game's
// rendering) while it is open. One at a time; the result is handed to `done` on that thread.
class FileDialog {
public:
    ~FileDialog() { Close(); }

    bool Busy() const { return busy_; }

    void Open(bool save, std::filesystem::path folder, std::wstring name,
              std::function<void(std::filesystem::path)> done)
    {
        if (busy_.exchange(true)) return;
        if (thread_.joinable()) thread_.join();
        thread_ = std::thread([this, save, folder = std::move(folder), name = std::move(name), done = std::move(done)] {
            threadId_ = GetCurrentThreadId();
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            const std::wstring dir = folder.wstring();

            wchar_t file[MAX_PATH] = {};
            wcsncpy_s(file, name.c_str(), _TRUNCATE);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = GameWindow();
            ofn.lpstrFilter = L"Macro / replay (*.merp)\0*.merp\0Macro as JSON (*.json)\0*.json\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrInitialDir = dir.c_str();
            ofn.lpstrDefExt = L"merp";
            ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
            ofn.lpstrTitle = save ? L"Save macro" : L"Open macro";
            // Pick JSON as the type when the default name asks for it, so the extension isn't forced to .merp.
            if (name.size() > 5 && name.compare(name.size() - 5, 5, L".json") == 0) {
                ofn.nFilterIndex = 2;
                ofn.lpstrDefExt = L"json";
            }
            const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
            threadId_ = 0;
            if (ok && !closing_) done(std::filesystem::path(file));
            busy_ = false;
        });
    }

    // Closes an open dialog (unload) and waits for its thread.
    void Close()
    {
        closing_ = true;
        if (const DWORD id = threadId_)
            EnumThreadWindows(id, [](HWND hwnd, LPARAM) -> BOOL { PostMessageW(hwnd, WM_CLOSE, 0, 0); return TRUE; }, 0);
        if (thread_.joinable()) thread_.join();
        closing_ = false;
    }

private:
    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> closing_{false};
    std::atomic<DWORD> threadId_{0};
};

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
        command.VisibleIf([this] { return start.Get() == 1; });
        level.VisibleIf([this] { return start.Get() == 2; });
        realtimeCap.VisibleIf([this] { return rate.Get() == 0; });
        speed.VisibleIf([this] { return !maxSpeed.Get(); });
        rewindSeconds.VisibleIf([this] { return start.Get() != 0; });
        onDeath.VisibleIf([this] { return start.Get() != 0; });
    }

    void OnDisable() override
    {
        tas::Stop();
        dialog_.Close();
    }

    void OnRender() override
    {
        tas::Options o;
        o.recordHz = kRates[std::clamp(rate.Get(), 0, static_cast<int>(std::size(kRates)) - 1)];
        o.realtimeCap = realtimeCap;
        o.speed = maxSpeed ? 0.f : speed.Get();
        o.pauseOnDesync = pauseOnDesync;
        o.blockStreaming = blockStreaming;
        o.seekWithoutRendering = seekNoRender;
        o.atEnd = static_cast<tas::AtEnd>(atEnd.Get());
        o.keyPause = pauseKey;
        o.keyAdvance = advanceKey;
        o.keyTakeOver = takeOverKey;
        o.keyStop = stopKey;
        o.keyRewind = rewindKey;
        o.onDeath = static_cast<tas::OnDeath>(onDeath.Get());
        o.rewindSeconds = rewindSeconds;
        tas::SetOptions(o);
        TakeDialogResult();

        status_ = tas::GetStatus();
        if (showOverlay) DrawOverlay();
        if (showEditor) DrawEditor();
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

    // Like Mega Hack's replay bot: one switch for the mode (Off / Record / Play), the macro's name with Save and Load
    // right under it, and practice mode / frame advance while it runs.
    void DrawControls(float width)
    {
        const tas::Status& st = status_;
        if (!st.available) {
            ImGui::TextWrapped("Unavailable: the engine hooks were not found (see the log).");
            return;
        }
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float half = (width - spacing) * 0.5f;
        const float third = (width - 2 * spacing) / 3.f;
        const bool idle = st.mode == tas::Mode::Idle;

        if (ModeButton("Off", idle, third, IM_COL32(90, 90, 90, 255))) tas::Stop();
        ImGui::SetItemTooltip("Stop recording / playing (%s)", KeyLabel(stopKey).c_str());
        ImGui::SameLine();
        if (ModeButton("Record", st.mode == tas::Mode::Recording, third, IM_COL32(190, 50, 45, 255))) StartRecording();
        ImGui::SetItemTooltip("A new recording, from the start set under \"New recording\". Replaces the macro in memory");
        ImGui::SameLine();
        if (ModeButton("Play", st.mode == tas::Mode::Playing, third, IM_COL32(45, 140, 70, 255))) tas::Play();
        ImGui::SetItemTooltip("Play the macro from its start. Your own input is ignored meanwhile");

        DrawMacroName(width);

        ImGui::Spacing();
        ImGui::TextUnformatted(StatusLine().c_str());
        if (!st.message.empty()) ImGui::TextDisabled("%s", st.message.c_str());
        if (st.desyncFrame >= 0)
            ImGui::TextColored({1.f, 0.4f, 0.3f, 1.f}, "Desync at frame %lld", st.desyncFrame);
        else if (st.verified)
            ImGui::TextColored({0.4f, 1.f, 0.5f, 1.f}, "Verified: every frame matched");

        ImGui::BeginDisabled(idle);
        if (ImGui::Button(st.paused ? "Resume" : "Pause", {half, 0})) tas::TogglePause();
        ImGui::SetItemTooltip("%s", KeyLabel(pauseKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Frame advance", {half, 0})) tas::Advance();
        ImGui::SetItemTooltip("Pauses, then runs one frame per click (%s)", KeyLabel(advanceKey).c_str());
        ImGui::BeginDisabled(st.mode != tas::Mode::Recording);
        if (ImGui::Button("Rewind", {half, 0})) tas::Rewind();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Practice: go back %.1f s and keep recording from there (%s)", rewindSeconds.Get(),
                              KeyLabel(rewindKey).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(st.mode != tas::Mode::Playing);
        if (ImGui::Button("Take over", {half, 0})) tas::TakeOver();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Keep this playback up to here and record from this frame on (%s)",
                              KeyLabel(takeOverKey).c_str());
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(half);
        ImGui::InputInt("##goto", &gotoFrame_, 0, 0);
        gotoFrame_ = std::max(gotoFrame_, 0);
        ImGui::SameLine();
        if (ImGui::Button("Go to frame", {half, 0})) tas::Goto(static_cast<std::size_t>(gotoFrame_));

        DrawGameplayWarning();
    }

    // The macro's name, and Save / Load into the macros folder under that name (Mega Hack's layout).
    void DrawMacroName(float width)
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float button = (width - spacing) * 0.25f;
        ImGui::SetNextItemWidth(width - 2 * (button + spacing));
        ImGui::InputTextWithHint("##file", "macro name", fileBuf_, sizeof fileBuf_);
        const bool named = fileBuf_[0] != 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(!named);
        if (ImGui::Button("Save", {button, 0})) SaveNamed();
        ImGui::SetItemTooltip("Save to replays/%s%s (next to the menu DLL)", fileBuf_, tas::kFileExtension);
        ImGui::SameLine();
        if (ImGui::Button("Load", {button, 0})) LoadPath(FilePath());
        ImGui::EndDisabled();
    }
    // Modules that change the game make a recording unverifiable (and its playback desync when they're off).
    void DrawGameplayWarning()
    {
        std::string on;
        modules::ForEach([&](modules::Tab& tab, Module& m) {
            if (!m.Enabled() || &m == this) return;
            if (tab.name != "Player" && tab.name != "World" && tab.name != "Trainer") return;
            if (m.Name() == "Tool Overlay" || m.Name() == "Speedometer") return;
            if (!on.empty()) on += ", ";
            on += m.Name();
        });
        if (!on.empty()) ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Changes the game: %s", on.c_str());
    }

    std::string StatusLine() const
    {
        const tas::Status& st = status_;
        char buf[128];
        switch (st.mode) {
            case tas::Mode::Idle:
                std::snprintf(buf, sizeof buf, "Idle, %zu frames loaded", st.frames);
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
        std::string name;                  // file name without extension
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time;
    };

    void RefreshFiles()
    {
        files_.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(tas::Folder(), ec)) {
            if (e.path().extension() != tas::kFileExtension) continue;
            MacroFile f;
            f.name = e.path().stem().string();
            f.size = e.file_size(ec);
            f.time = e.last_write_time(ec);
            files_.push_back(std::move(f));
        }
        // Newest first, like a recent list
        std::sort(files_.begin(), files_.end(), [](const MacroFile& a, const MacroFile& b) { return a.time > b.time; });
        filesLoaded_ = true;
    }

    std::filesystem::path FilePath() const { return tas::Folder() / (std::string(fileBuf_) + tas::kFileExtension); }

    void SaveNamed()
    {
        std::string error;
        tas::WithReplay([&](tas::Replay& r) {
            if (r.description.empty()) r.description = fileBuf_;
        });
        if (tas::SaveFile(FilePath(), &error)) notify::Push("Saved " + FilePath().filename().string());
        else notify::Push("TAS: " + error);
        RefreshFiles();
    }

    void LoadPath(const std::filesystem::path& path)
    {
        std::string error;
        if (tas::LoadFile(path, &error)) {
            notify::Push("Loaded " + path.filename().string());
            std::snprintf(fileBuf_, sizeof fileBuf_, "%s", path.stem().string().c_str());
        } else {
            notify::Push("TAS: " + error);
        }
    }

    // Results of the file dialog arrive on its thread: handled here, on the render thread.
    void TakeDialogResult()
    {
        std::filesystem::path path;
        bool save = false;
        {
            std::lock_guard lock(dialogMutex_);
            if (dialogResult_.empty()) return;
            path = std::move(dialogResult_);
            dialogResult_.clear();
            save = dialogSave_;
        }
        if (save) {
            std::string error;
            if (tas::SaveFile(path, &error)) notify::Push("Saved " + path.filename().string());
            else notify::Push("TAS: " + error);
            RefreshFiles();
        } else {
            LoadPath(path);
        }
    }

    void OpenDialog(bool save, bool json = false)
    {
        std::wstring name;
        if (save) {
            const std::string base = fileBuf_[0] ? fileBuf_ : "macro";
            name = std::filesystem::path(base + (json ? ".json" : tas::kFileExtension)).wstring();
        }
        dialog_.Open(save, tas::Folder(), name, [this, save](std::filesystem::path path) {
            std::lock_guard lock(dialogMutex_);
            dialogResult_ = std::move(path);
            dialogSave_ = save;
        });
    }

    void DrawFiles(float width)
    {
        if (!filesLoaded_) RefreshFiles();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float half = (width - spacing) * 0.5f;
        const float third = (width - 2 * spacing) / 3.f;

        // Any file, anywhere: Windows' Open / Save As dialog
        ImGui::BeginDisabled(dialog_.Busy());
        if (ImGui::Button("Save to file...", {half, 0})) OpenDialog(true);
        ImGui::SetItemTooltip("Save the macro anywhere (.merp, or .json to get readable text)");
        ImGui::SameLine();
        if (ImGui::Button("Load from file...", {half, 0})) OpenDialog(false);
        ImGui::SetItemTooltip("Open a .merp or .json macro from anywhere");
        ImGui::EndDisabled();
        if (dialog_.Busy()) ImGui::TextDisabled("The file dialog is open (Alt+Tab if you can't see it)");

        // The macros folder, newest first
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##search", "search macros", searchBuf_, sizeof searchBuf_);
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginListBox("##macros", {width, rowH * 6.f + 4.f})) {
            int shown = 0;
            for (const auto& f : files_) {
                if (!ContainsNoCase(f.name, searchBuf_)) continue;
                ++shown;
                const bool selected = f.name == fileBuf_;
                if (ImGui::Selectable(f.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    std::snprintf(fileBuf_, sizeof fileBuf_, "%s", f.name.c_str());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) LoadPath(FilePath());
                }
                ImGui::SetItemTooltip("Double click to load (%.1f KB)", static_cast<double>(f.size) / 1024.0);
            }
            if (shown == 0) ImGui::TextDisabled(files_.empty() ? "No saved macros yet" : "No match");
            ImGui::EndListBox();
        }

        const bool named = fileBuf_[0] != 0;
        if (ImGui::Button("Refresh", {third, 0})) RefreshFiles();
        ImGui::SameLine();
        ImGui::BeginDisabled(!named);
        if (ImGui::Button(confirmDelete_ ? "Sure?" : "Delete", {third, 0})) {
            if (!confirmDelete_) {
                confirmDelete_ = true;
            } else {
                confirmDelete_ = false;
                std::error_code ec;
                if (std::filesystem::remove(FilePath(), ec)) notify::Push("Deleted " + FilePath().filename().string());
                else notify::Push("TAS: couldn't delete " + FilePath().filename().string());
                RefreshFiles();
            }
        }
        if (!ImGui::IsItemHovered()) confirmDelete_ = false;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("New", {third, 0})) {
            tas::Clear();
            fileBuf_[0] = 0;
        }
        ImGui::SetItemTooltip("Stop and empty the macro in memory (saved files are kept)");

        ImGui::BeginDisabled(dialog_.Busy());
        if (ImGui::Button("Export JSON...", {width, 0})) OpenDialog(true, true);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The macro as readable, editable text. Load from file... reads it back");

        DrawInfo();
    }

    void DrawInfo()
    {
        tas::WithReplay([&](tas::Replay& r) {
            ImGui::TextDisabled("%zu frames, %s, %zu inputs, %u rerecords", r.frames.size(),
                                FormatTime(r.Duration()).c_str(), r.events.size(), r.rerecords);
            ImGui::TextDisabled("%s, %s", r.fixedHz > 0.f ? (std::to_string(static_cast<int>(r.fixedHz)) + " fps").c_str()
                                                          : "real time",
                                r.start == tas::StartKind::Now ? "from anywhere"
                                : r.start == tas::StartKind::Command ? r.command.c_str()
                                : (r.map + (r.checkpoint.empty() ? "" : " @ " + r.checkpoint)).c_str());
            ImGui::TextDisabled("input id %016llx", static_cast<unsigned long long>(r.InputHash()));
            ImGui::SetItemTooltip("Same id = same start and inputs. The sync id is the recorded result.");
            ImGui::TextDisabled("sync id  %016llx", static_cast<unsigned long long>(r.SyncHash()));

            char author[64], desc[256];
            std::snprintf(author, sizeof author, "%s", r.author.c_str());
            std::snprintf(desc, sizeof desc, "%s", r.description.c_str());
            if (ImGui::InputTextWithHint("##author", "author", author, sizeof author)) r.author = author;
            if (ImGui::InputTextWithHint("##desc", "description", desc, sizeof desc)) r.description = desc;
        });
    }

    void DrawTranslator(float width)
    {
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

    // ---- editor ------------------------------------------------------------------------------------------------------------
    void DrawEditor()
    {
        ImGui::SetNextWindowSize({620.f, 460.f}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (!ImGui::Begin("Replay Editor", &open)) {
            ImGui::End();
            if (!open) showEditor.Set(false);
            return;
        }
        if (!open) showEditor.Set(false);

        const tas::Status st = status_;
        const bool readOnly = st.mode == tas::Mode::Recording;
        ImGui::Checkbox("Follow playback", &follow_);
        ImGui::SameLine();
        if (readOnly) ImGui::TextDisabled("(read only while recording)");

        tas::WithReplay([&](tas::Replay& r) {
            if (selected_ >= static_cast<long long>(r.frames.size())) selected_ = static_cast<long long>(r.frames.size()) - 1;

            const float listWidth = ImGui::GetContentRegionAvail().x * 0.55f;
            ImGui::BeginChild("frames", {listWidth, 0}, ImGuiChildFlags_Borders);
            DrawFrameList(r, st);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("frame", {0, 0}, ImGuiChildFlags_Borders);
            ImGui::BeginDisabled(readOnly);
            DrawFrameDetails(r);
            ImGui::EndDisabled();
            ImGui::EndChild();
        });
        ImGui::End();
    }

    void DrawFrameList(tas::Replay& r, const tas::Status& st)
    {
        if (ImGui::BeginTable("frames", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Frame");
            ImGui::TableSetupColumn("ms");
            ImGui::TableSetupColumn("Inputs", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            const float rowH = ImGui::GetTextLineHeightWithSpacing();
            const bool running = st.mode != tas::Mode::Idle && !st.armed;
            if (follow_ && running && st.frame != lastFollowed_) {
                ImGui::SetScrollY(std::max(0.f, (static_cast<float>(st.frame) - 5.f) * rowH));
                lastFollowed_ = st.frame;
            }

            ImGuiListClipper clipper;      // only the visible rows are built: hours of frames cost nothing
            clipper.Begin(static_cast<int>(r.frames.size()), rowH);
            std::string inputs;
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const tas::Frame& f = r.frames[i];
                    ImGui::TableNextRow();
                    if (running && static_cast<std::size_t>(i) == st.frame)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 110, 200, 120));
                    if (st.desyncFrame == i)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(200, 60, 50, 140));

                    ImGui::TableNextColumn();
                    char label[16];
                    std::snprintf(label, sizeof label, "%d", i);
                    if (ImGui::Selectable(label, selected_ == i, ImGuiSelectableFlags_SpanAllColumns)) selected_ = i;
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", f.dt * 1000.f);
                    ImGui::TableNextColumn();
                    inputs.clear();
                    for (std::uint32_t k = 0; k < f.count && inputs.size() < 120; ++k) {
                        if (k) inputs += ", ";
                        inputs += tas::Describe(r, r.events[f.first + k]);
                    }
                    ImGui::TextUnformatted(inputs.c_str());
                }
            }
            ImGui::EndTable();
        }
    }

    void DrawFrameDetails(tas::Replay& r)
    {
        if (ImGui::Button("+ Frame")) {
            const std::size_t at = selected_ >= 0 ? static_cast<std::size_t>(selected_) + 1 : r.frames.size();
            r.InsertFrames(at, static_cast<std::size_t>(frameCount_), DefaultDt(r));
            selected_ = static_cast<long long>(at);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.f);
        ImGui::InputInt("##n", &frameCount_, 0, 0);
        frameCount_ = std::clamp(frameCount_, 1, 100000);
        ImGui::SetItemTooltip("How many frames + Frame / Delete add or remove");

        if (selected_ < 0 || selected_ >= static_cast<long long>(r.frames.size())) {
            ImGui::TextDisabled("Select a frame");
            return;
        }
        const auto fi = static_cast<std::size_t>(selected_);
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            r.EraseFrames(fi, static_cast<std::size_t>(frameCount_));
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Go to")) tas::Goto(fi);

        ImGui::Text("Frame %zu", fi);
        float ms = r.frames[fi].dt * 1000.f;
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::InputFloat("ms##dt", &ms, 0.f, 0.f, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue))
            r.SetFrameDt(fi, std::clamp(ms, 0.1f, 100.f) / 1000.f);
        ImGui::Separator();

        const tas::Frame f = r.frames[fi];
        for (std::uint32_t k = 0; k < f.count; ++k) {
            ImGui::PushID(static_cast<int>(k));
            tas::Event e = r.events[f.first + k];
            bool changed = false;

            char name[48];
            std::snprintf(name, sizeof name, "%s", r.Name(e.name).c_str());
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
                e.name = r.NameIndex(name);
                changed = true;
            }
            ImGui::SameLine();
            static const char* kinds[] = {"down", "up", "repeat", "double", "axis"};
            int kind = e.kind == tas::EventKind::Axis ? 4 : static_cast<int>(e.kind);
            ImGui::SetNextItemWidth(70.f);
            if (ImGui::Combo("##kind", &kind, kinds, 5)) {
                e.kind = kind == 4 ? tas::EventKind::Axis : static_cast<tas::EventKind>(kind);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            changed |= ImGui::DragFloat("##v", &e.value, 0.05f, 0.f, 0.f, "%.3f");
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                r.EraseEvent(fi, k);
                ImGui::PopID();
                break;
            }
            if (changed) r.ReplaceEvent(fi, k, e);
            ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::SetNextItemWidth(100.f);
        ImGui::InputTextWithHint("##newname", "key / MouseX", newName_, sizeof newName_);
        ImGui::SameLine();
        if (ImGui::Button("Down")) AddEvent(r, fi, tas::EventKind::Pressed);
        ImGui::SameLine();
        if (ImGui::Button("Up")) AddEvent(r, fi, tas::EventKind::Released);
        ImGui::SameLine();
        if (ImGui::Button("Axis")) AddEvent(r, fi, tas::EventKind::Axis);
        ImGui::TextDisabled("Unreal key names: W, SpaceBar, LeftMouseButton, MouseX...");
    }

    void AddEvent(tas::Replay& r, std::size_t frame, tas::EventKind kind)
    {
        if (!newName_[0]) return;
        tas::Event e;
        e.name = r.NameIndex(newName_);
        e.kind = kind;
        e.value = kind == tas::EventKind::Axis ? 0.f : 1.f;
        e.value2 = kind == tas::EventKind::Axis ? r.frames[frame].dt : 0.f;
        r.InsertEvent(frame, r.frames[frame].count, e);
    }

    static float DefaultDt(const tas::Replay& r)
    {
        return static_cast<float>(1.0 / (r.fixedHz > 0.f ? r.fixedHz : 60.f));
    }

    // ---- state ------------------------------------------------------------------------------------------------------------
    tas::Status status_;
    char commandBuf_[128] = "open edge_p";
    char fileBuf_[96] = "";
    char fromBuf_[48] = "", toBuf_[48] = "", newName_[48] = "";
    int map_ = 0, checkpoint_ = 0;
    int gotoFrame_ = 0;
    int frameCount_ = 1;
    long long selected_ = -1;
    bool follow_ = true;
    std::size_t lastFollowed_ = static_cast<std::size_t>(-1);
    std::vector<MacroFile> files_;
    bool filesLoaded_ = false;
    char searchBuf_[64] = "";
    bool confirmDelete_ = false;
    FileDialog dialog_;
    std::mutex dialogMutex_;
    std::filesystem::path dialogResult_;
    bool dialogSave_ = false;

    // ---- controls (drawn in this order) -----------------------------------------------------------------------------------------
    ctl::Custom controls{this, [this](float w) { DrawControls(w); }};

    ctl::Separator recordTitle{this, "New recording"};
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

    ctl::Separator playTitle{this, "Playback"};
    ctl::Slider speed{this, "Speed", 1.f, 0.05f, 8.f, "%.2fx", 0.05f};
    ctl::Checkbox maxSpeed{this, "As fast as possible", false};
    ctl::Combo atEnd{this, "At the end", {"Stop", "Pause", "Keep recording"}, 1};
    ctl::Checkbox pauseOnDesync{this, "Pause on desync", true};
    ctl::Checkbox blockStreaming{this, "Deterministic streaming", true,
                                 "Load streamed level parts in the frame they are requested (a short hitch, but every run "
                                 "and every PC gets them on the same frame). Recordings need the same setting to replay"};
    ctl::Checkbox seekNoRender{this, "Don't render while seeking", false,
                               "Faster Go to frame. Animations of unseen things may update differently: can desync"};

    ctl::Separator filesTitle{this, "Macros"};
    ctl::Custom files{this, [this](float w) { DrawFiles(w); }};

    ctl::Separator keysTitle{this, "Keys"};
    ctl::KeyHold pauseKey{this, "Pause / resume", VK_F5};
    ctl::KeyHold advanceKey{this, "Frame advance", VK_F6};
    ctl::KeyHold takeOverKey{this, "Take over", VK_F7};
    ctl::KeyHold stopKey{this, "Stop", VK_F8};
    ctl::KeyHold rewindKey{this, "Rewind", VK_F9};

    ctl::Separator toolsTitle{this, "Tools"};
    ctl::Checkbox showOverlay{this, "Status overlay", true};
    ctl::Checkbox showEditor{this, "Replay editor", false};
    ctl::Custom translator{this, [this](float w) { DrawTranslator(w); }};
};
MEM_MODULE(Tas)

} // namespace
