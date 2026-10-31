#include "core/module.h"
#include "core/notify.h"
#include "game/levels.h"
#include "tas/tas.h"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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

class Tas final : public Module {
public:
    Tas() : Module("TAS / Replay", "TAS", "Record, play back, verify and edit runs (.merp)", 10)
    {
        enabledByDefault = true;
        expanded = true;
        command.VisibleIf([this] { return start.Get() == 1; });
        level.VisibleIf([this] { return start.Get() == 2; });
        realtimeCap.VisibleIf([this] { return rate.Get() == 0; });
        speed.VisibleIf([this] { return !maxSpeed.Get(); });
    }

    void OnDisable() override { tas::Stop(); }

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
        tas::SetOptions(o);

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
            for (int i = 0; i < static_cast<int>(maps.size()); ++i)
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

    void DrawControls(float width)
    {
        const tas::Status& st = status_;
        if (!st.available) {
            ImGui::TextWrapped("Unavailable: the engine hooks were not found (see the log).");
            return;
        }
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool idle = st.mode == tas::Mode::Idle;

        if (ImGui::Button("Record", {half, 0})) StartRecording();
        ImGui::SameLine();
        if (ImGui::Button("Play", {half, 0})) tas::Play();

        ImGui::BeginDisabled(idle);
        if (ImGui::Button(st.paused ? "Resume" : "Pause", {half, 0})) tas::TogglePause();
        ImGui::SameLine();
        if (ImGui::Button("Advance", {half, 0})) tas::Advance();
        ImGui::BeginDisabled(st.mode != tas::Mode::Playing);
        if (ImGui::Button("Take over", {half, 0})) tas::TakeOver();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Keep this playback up to here and record from this frame on (a rerecord)");
        ImGui::SameLine();
        if (ImGui::Button("Stop", {half, 0})) tas::Stop();
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(half);
        ImGui::InputInt("##goto", &gotoFrame_, 0, 0);
        gotoFrame_ = std::max(gotoFrame_, 0);
        ImGui::SameLine();
        if (ImGui::Button("Go to frame", {half, 0})) tas::Goto(static_cast<std::size_t>(gotoFrame_));

        ImGui::Spacing();
        ImGui::TextUnformatted(StatusLine().c_str());
        if (!st.message.empty()) ImGui::TextDisabled("%s", st.message.c_str());
        if (st.desyncFrame >= 0)
            ImGui::TextColored({1.f, 0.4f, 0.3f, 1.f}, "Desync at frame %lld", st.desyncFrame);
        else if (st.verified)
            ImGui::TextColored({0.4f, 1.f, 0.5f, 1.f}, "Verified: every frame matched");

        DrawGameplayWarning();
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

    void RefreshFiles()
    {
        files_.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(tas::Folder(), ec))
            if (e.path().extension() == tas::kFileExtension) files_.push_back(e.path().stem().string());
        std::sort(files_.begin(), files_.end());
        filesLoaded_ = true;
    }

    std::filesystem::path FilePath() const { return tas::Folder() / (std::string(fileBuf_) + tas::kFileExtension); }

    void DrawFiles(float width)
    {
        if (!filesLoaded_) RefreshFiles();
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        std::string error;

        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##file", "file name", fileBuf_, sizeof fileBuf_);
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##files", "Saved replays")) {
            for (const auto& f : files_)
                if (ImGui::Selectable(f.c_str())) std::snprintf(fileBuf_, sizeof fileBuf_, "%s", f.c_str());
            ImGui::EndCombo();
        }
        if (ImGui::IsItemClicked()) RefreshFiles();

        const bool named = fileBuf_[0] != 0;
        ImGui::BeginDisabled(!named);
        if (ImGui::Button("Save", {half, 0})) {
            if (tas::SaveFile(FilePath(), &error)) notify::Push("Saved " + FilePath().filename().string());
            RefreshFiles();
        }
        ImGui::SameLine();
        if (ImGui::Button("Load", {half, 0}) && tas::LoadFile(FilePath(), &error))
            notify::Push("Loaded " + FilePath().filename().string());
        if (ImGui::Button("Export JSON", {width, 0})) {
            auto path = FilePath();
            path.replace_extension(".merp.json");
            bool ok = false;
            tas::WithReplay([&](tas::Replay& r) { ok = r.ExportJson(path, &error); });
            if (ok) notify::Push("Exported " + path.filename().string());
        }
        ImGui::EndDisabled();
        if (!error.empty()) notify::Push("TAS: " + error);

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
    std::vector<std::string> files_;
    bool filesLoaded_ = false;

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

    ctl::Separator filesTitle{this, "File (.merp)"};
    ctl::Custom files{this, [this](float w) { DrawFiles(w); }};

    ctl::Separator keysTitle{this, "Keys"};
    ctl::KeyHold pauseKey{this, "Pause / resume", VK_F5};
    ctl::KeyHold advanceKey{this, "Frame advance", VK_F6};
    ctl::KeyHold takeOverKey{this, "Take over", VK_F7};
    ctl::KeyHold stopKey{this, "Stop", VK_F8};

    ctl::Separator toolsTitle{this, "Tools"};
    ctl::Checkbox showOverlay{this, "Status overlay", true};
    ctl::Checkbox showEditor{this, "Replay editor", false};
    ctl::Custom translator{this, [this](float w) { DrawTranslator(w); }};
};
MEM_MODULE(Tas)

} // namespace
