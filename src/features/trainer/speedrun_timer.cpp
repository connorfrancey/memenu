// Speedrun timer: a load removed timer with auto splits on chapter ends and personal best comparison, in the spirit of
// softsoundd's Softimer (Mirror's Edge Tweaks). The run logic is speedrun/live.cpp; this is the HUD and its settings.
#include "core/keys.h"
#include "core/module.h"
#include "gui.h"
#include "speedrun/live.h"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using speedrun::Timer;

ImU32 Col(const std::array<float, 4>& c, float alpha = 1.f)
{
    return ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3] * alpha});
}

// Text right-aligned in what's left of the current line / cell.
void RightText(ImU32 color, const std::string& s)
{
    const float w = ImGui::CalcTextSize(s.c_str()).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
}

class SpeedrunTimer final : public Module {
public:
    SpeedrunTimer() : Module("Speedrun Timer", "Trainer", "Load removed timer with auto splits and personal bests", 75)
    {
        const auto in = [](ctl::Section& section, std::initializer_list<ctl::Setting*> controls) {
            for (ctl::Setting* c : controls) c->VisibleIf([&section] { return section.Get(); });
        };
        in(runSection, {&category, &controls});
        in(autoSection, {&autoStart, &autoSplit, &autoFinish, &autoReset, &removeLoads, &pauseInMenus});
        in(showSection, {&showSplits, &showSegment, &showLastSplit, &showRta, &showPb, &showSob, &simple, &decimals,
                         &hideWhileRunning});
        splitRows.VisibleIf([this] { return showSection && showSplits; });
        in(lookSection, {&width, &timerSize, &opacity, &titleBar, &aheadColor, &behindColor, &goldColor});
        in(keysSection, {&splitKey, &resetKey, &undoKey, &skipKey});
        in(pbSection, {&pbInfo});
    }

    void OnTick() override
    {
        speedrun::SetOptions(Options(true));
        speedrun::Tick();
    }

    void OnDisable() override { speedrun::SetOptions(Options(false)); }

    void OnRender() override
    {
        view_ = speedrun::Snapshot();
        const auto& v = view_;
        const auto phase = v.timer.GetPhase();
        if (hideWhileRunning && phase == Timer::Phase::Running && !gui::visible) return;

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoScrollbar;
        if (!titleBar) flags |= ImGuiWindowFlags_NoTitleBar;
        if (!gui::visible) flags |= ImGuiWindowFlags_NoInputs;     // only movable while the menu is open

        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({screen.x - width - 30.f, 40.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({width, 0.f}, ImGuiCond_Always);   // height 0: fits the content
        ImGui::SetNextWindowBgAlpha(opacity);
        if (ImGui::Begin("Speedrun timer##speedrun", nullptr, flags)) {
            if (showSplits) DrawSplits(v);
            DrawTimer(v);
            DrawInfo(v);
        }
        ImGui::End();
    }

private:
    speedrun::Options Options(bool enabled) const
    {
        speedrun::Options o;
        o.enabled = enabled;
        o.category = static_cast<speedrun::Category>(category.Get());
        o.autoStart = autoStart;
        o.autoSplit = autoSplit;
        o.autoFinish = autoFinish;
        o.autoReset = autoReset;
        o.removeLoads = removeLoads;
        o.pauseInMenus = pauseInMenus;
        o.keySplit = splitKey;
        o.keyReset = resetKey;
        o.keyUndo = undoKey;
        o.keySkip = skipKey;
        return o;
    }

    double Lrt(const speedrun::View& v) const { return v.timer.Lrt(speedrun::Now()); }

    // Where the run stands against the personal best: < 0 ahead, > 0 behind, NaN without a comparison.
    double LiveDelta(const speedrun::View& v) const
    {
        const auto& segs = v.comparison.segments;
        const std::size_t i = v.timer.Splits().size();
        if (v.timer.GetPhase() == Timer::Phase::Finished) {
            const double pb = v.newPb ? -1.0 : v.comparison.PbTime();
            return pb >= 0.0 ? v.timer.FinalLrt() - pb : (v.newPb ? -1.0 : std::nan(""));
        }
        if (i >= segs.size() || segs[i].pb < 0.0) return std::nan("");
        return Lrt(v) - segs[i].pb;
    }

    ImU32 DeltaColor(double delta, double previous) const
    {
        const bool ahead = delta < 0.0;
        // Losing time while ahead / gaining while behind: the lighter shade (like LiveSplit)
        const bool lighter = !std::isnan(previous) && (ahead ? delta > previous : delta < previous);
        const auto& c = ahead ? aheadColor.Get() : behindColor.Get();
        if (!lighter) return Col(c);
        return ImGui::ColorConvertFloat4ToU32({c[0] + (1.f - c[0]) * 0.45f, c[1] + (1.f - c[1]) * 0.45f,
                                               c[2] + (1.f - c[2]) * 0.45f, c[3]});
    }

    void DrawSplits(const speedrun::View& v)
    {
        const auto& segs = v.comparison.segments;
        const auto& run = v.timer.Splits();
        if (segs.empty()) return;
        const int d = decimals;
        const bool running = v.timer.Running();
        const int current = running ? static_cast<int>(run.size()) : -1;

        // A window of rows that keeps the current split in view, with the last split always at the bottom.
        const int total = static_cast<int>(segs.size());
        const int rows = std::min(total, std::max(2, splitRows.Get()));
        const int focus = current >= 0 ? current : static_cast<int>(run.size()) - 1;
        const int first = std::clamp(focus - rows + 3, 0, total - rows);

        ImGui::TextDisabled("%s", v.categoryLabel.c_str());
        ImGui::SameLine();
        RightText(ImGui::GetColorU32(ImGuiCol_TextDisabled), std::to_string(v.comparison.attempts));

        if (!ImGui::BeginTable("##splits", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) return;
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("delta", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("-0:00.00").x);
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0:00:00.00").x);

        double previousDelta = std::nan("");
        for (int i = 0; i < total; ++i) {
            const bool shown = (i >= first && i < first + rows - 1) || i == total - 1;
            const speedrun::Segment& s = segs[i];
            double delta = std::nan("");
            if (i < static_cast<int>(run.size()) && run[i].time >= 0.0 && s.pb >= 0.0) delta = run[i].time - s.pb;
            if (shown) DrawSplitRow(v, i, current, delta, previousDelta, d);
            if (!std::isnan(delta)) previousDelta = delta;
        }
        ImGui::EndTable();
        ImGui::Separator();
    }

    void DrawSplitRow(const speedrun::View& v, int i, int current, double delta, double previousDelta, int d)
    {
        const auto& s = v.comparison.segments[i];
        const auto& run = v.timer.Splits();
        const bool done = i < static_cast<int>(run.size());
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        ImGui::TableNextRow();
        if (i == current) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header, 0.8f));

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, i == current ? text : (done ? text : dim));
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        if (done) {
            const double seg = speedrun::Comparison::SegmentTime(run, i);
            const bool gold = seg >= 0.0 && s.best >= 0.0 && seg < s.best;
            if (!std::isnan(delta)) RightText(gold ? Col(goldColor) : DeltaColor(delta, previousDelta),
                                              speedrun::FormatDelta(delta, std::min(d, 2)));
        } else if (i == current && s.pb >= 0.0) {
            // Live: only once behind the PB split (ahead isn't known until the split)
            const double live = Lrt(v) - s.pb;
            if (live > 0.0) RightText(Col(behindColor), speedrun::FormatDelta(live, std::min(d, 1)));
        }

        ImGui::TableNextColumn();
        if (done) RightText(text, run[i].time >= 0.0 ? speedrun::Format(run[i].time, std::min(d, 2)) : "-");
        else RightText(dim, s.pb >= 0.0 ? speedrun::Format(s.pb, std::min(d, 2)) : "-");
    }

    void DrawTimer(const speedrun::View& v)
    {
        const auto phase = v.timer.GetPhase();
        const double lrt = Lrt(v);
        const double delta = LiveDelta(v);
        ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        if (phase == Timer::Phase::Idle) color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        else if (phase == Timer::Phase::Finished && v.newPb) color = Col(goldColor);
        else if (!std::isnan(delta)) color = Col(delta < 0.0 ? aheadColor : behindColor);
        if (v.loading && phase == Timer::Phase::Running) color = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        std::string text = speedrun::Format(lrt, decimals);
        ImGui::PushFont(nullptr, timerSize);
        RightText(color, text);
        ImGui::PopFont();
        if (!simple) {
            // The label goes in front of the big digits, on their baseline
            const ImVec2 max = ImGui::GetItemRectMax();
            const char* label = v.loading && phase == Timer::Phase::Running ? "LOAD" : "LRT";
            const float big = ImGui::CalcTextSize(text.c_str()).x * timerSize / ImGui::GetFontSize();
            const ImVec2 at{max.x - big - ImGui::CalcTextSize(label).x - 8.f, max.y - ImGui::GetFontSize() - 4.f};
            ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
        }
    }

    void Line(const char* label, const std::string& value, ImU32 color)
    {
        if (!simple) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
        }
        RightText(color, value);
    }

    void DrawInfo(const speedrun::View& v)
    {
        const int d = std::min(decimals.Get(), 2);
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        const auto& run = v.timer.Splits();
        const auto& segs = v.comparison.segments;
        const auto phase = v.timer.GetPhase();

        if (showSegment && phase == Timer::Phase::Running) {
            const std::size_t i = run.size();
            const double seg = Lrt(v) - v.timer.SegmentStart();
            const bool slowerThanBest = i < segs.size() && segs[i].best >= 0.0 && seg > segs[i].best;
            Line(simple ? "" : (i < segs.size() ? segs[i].name.c_str() : "Segment"), speedrun::Format(seg, d),
                 slowerThanBest ? Col(behindColor, 0.85f) : text);
        }
        if (showLastSplit && !run.empty()) {
            const std::size_t i = run.size() - 1;
            const double seg = speedrun::Comparison::SegmentTime(run, i);
            const std::string label = simple ? std::string() : "Last: " + run[i].name;
            Line(label.c_str(), seg >= 0.0 ? speedrun::Format(seg, d) : "-", text);
        }
        if (showRta && phase != Timer::Phase::Idle) Line("RTA", speedrun::Format(v.timer.Rta(speedrun::Now()), d), text);
        if (showPb) {
            const double pb = v.comparison.PbTime();
            Line("Personal best", pb >= 0.0 ? speedrun::Format(pb, d) : "-", text);
        }
        if (showSob) {
            const double sob = v.comparison.SumOfBest();
            Line("Sum of best", sob >= 0.0 ? speedrun::Format(sob, d) : "-", text);
        }
        if (gui::visible && !v.hooked && autoSplit)
            ImGui::TextDisabled("auto splits not available (see the log)");
    }

    // Status and buttons, in the module's panel.
    void DrawControls(float w)
    {
        const auto& v = view_;
        const auto phase = v.timer.GetPhase();
        const char* state = phase == Timer::Phase::Idle ? "ready" : phase == Timer::Phase::Finished
                                ? (v.newPb ? "finished: personal best!" : "finished") : (v.loading ? "running (load)" : "running");
        ImGui::TextDisabled("%s, %s", v.categoryLabel.c_str(), state);
        if (!v.lastEvent.empty()) ImGui::TextDisabled("last: %s", v.lastEvent.c_str());

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float bw = (w - spacing) / 2.f;
        const char* first = phase == Timer::Phase::Running ? "Split" : "Start";
        if (ImGui::Button(first, {bw, 0.f})) speedrun::StartOrSplit();
        ImGui::SetItemTooltip("%s", keys::Name(splitKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Reset", {bw, 0.f})) speedrun::Reset();
        ImGui::SetItemTooltip("%s", keys::Name(resetKey).c_str());
        ImGui::BeginDisabled(phase == Timer::Phase::Idle);
        if (ImGui::Button("Undo split", {bw, 0.f})) speedrun::Undo();
        ImGui::SetItemTooltip("%s", keys::Name(undoKey).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Skip split", {bw, 0.f})) speedrun::SkipSplit();
        ImGui::SetItemTooltip("%s", keys::Name(skipKey).c_str());
        ImGui::EndDisabled();
        if (!v.hooked && Enabled())
            ImGui::TextColored({1.f, 0.7f, 0.3f, 1.f}, "Auto splits are off: the game's\nlevel completed event wasn't found.");
    }

    void DrawPbInfo(float w)
    {
        const auto& v = view_;
        const auto& c = v.comparison;
        const double pb = c.PbTime(), sob = c.SumOfBest();
        ImGui::Text("%s: %d attempts, %d finished", v.categoryLabel.c_str(), c.attempts, c.completed);
        ImGui::Text("Personal best  %s", pb >= 0.0 ? speedrun::Format(pb, 3).c_str() : "none");
        ImGui::Text("Sum of best    %s", sob >= 0.0 ? speedrun::Format(sob, 3).c_str() : "-");
        ImGui::TextDisabled("Saved in memenu\\speedrun\\");

        const bool confirm = ImGui::GetTime() < confirmUntil_;
        if (ImGui::Button(confirm ? "Sure? Click again" : "Forget personal best", {w, 0.f})) {
            if (confirm) {
                speedrun::ForgetPb();
                confirmUntil_ = 0.0;
            } else confirmUntil_ = ImGui::GetTime() + 3.0;
        }
        ImGui::SetItemTooltip("Clears the personal best and best segments of %s (not the attempt count)", v.categoryLabel.c_str());
    }

    speedrun::View view_;           // Render thread: the last snapshot
    double confirmUntil_ = 0.0;

    ctl::Section runSection{this, "Run", true};
    ctl::Combo category{this, "Category", speedrun::CategoryNames(), 0,
                        "Story categories start with a new game and end when Faith grabs the helicopter. Individual level: "
                        "starts when you enter a level and ends when it's completed. Each has its own personal best."};
    ctl::Custom controls{this, [this](float w) { DrawControls(w); }};

    ctl::Section autoSection{this, "Automatic", false};
    ctl::Checkbox autoStart{this, "Auto start", true, "Story: on a new game. Individual level: when a level loads."};
    ctl::Checkbox autoSplit{this, "Auto split", true, "When the game completes a chapter"};
    ctl::Checkbox autoFinish{this, "Auto stop", true, "Story: when Faith grabs the helicopter. Individual level: when the level is completed."};
    ctl::Checkbox autoReset{this, "Auto reset", true,
                            "Story: on a new game. Individual level: whenever the level is loaded again (restart, checkpoint)."};
    ctl::Checkbox removeLoads{this, "Remove loads", true,
                              "The timer stands still while a level loads and while the game waits on streaming"};
    ctl::Checkbox pauseInMenus{this, "Stop in menus", true,
                               "The timer stands still while the game is paused and in the main menu"};

    ctl::Section showSection{this, "Show", false};
    ctl::Checkbox showSplits{this, "Splits", true};
    ctl::SliderInt splitRows{this, "Split rows", 10, 2, 12};
    ctl::Checkbox showSegment{this, "Segment timer", true, "Time in the current chapter"};
    ctl::Checkbox showLastSplit{this, "Last split", false, "How long the last chapter took"};
    ctl::Checkbox showRta{this, "Real time", false, "Loads included"};
    ctl::Checkbox showPb{this, "Personal best", false};
    ctl::Checkbox showSob{this, "Sum of best", false};
    ctl::Checkbox simple{this, "Simple", false, "No labels, just the times"};
    ctl::SliderInt decimals{this, "Decimals", 2, 0, 3};
    ctl::Checkbox hideWhileRunning{this, "Hide while running", false, "Only show the timer when no run is going (or the menu is open)"};

    ctl::Section lookSection{this, "Look", false};
    ctl::Slider width{this, "Width", 250.f, 160.f, 500.f, "%.0f", 10.f};
    ctl::Slider timerSize{this, "Timer size", 40.f, 16.f, 96.f, "%.0f px", 2.f};
    ctl::Slider opacity{this, "Opacity", 0.55f, 0.f, 1.f, "%.2f", 0.05f};
    ctl::Checkbox titleBar{this, "Title bar", false};
    ctl::ColorPick aheadColor{this, "Ahead", {0.30f, 0.85f, 0.45f, 1.f}};
    ctl::ColorPick behindColor{this, "Behind", {0.95f, 0.35f, 0.35f, 1.f}};
    ctl::ColorPick goldColor{this, "Best segment", {1.f, 0.82f, 0.25f, 1.f}};

    ctl::Section keysSection{this, "Keys", false};
    ctl::KeyHold splitKey{this, "Start / split", VK_NUMPAD1};
    ctl::KeyHold resetKey{this, "Reset", VK_NUMPAD3};
    ctl::KeyHold undoKey{this, "Undo split", VK_NUMPAD8};
    ctl::KeyHold skipKey{this, "Skip split", VK_NUMPAD2};

    ctl::Section pbSection{this, "Records", false};
    ctl::Custom pbInfo{this, [this](float w) { DrawPbInfo(w); }};
};
MEM_MODULE(SpeedrunTimer)

} // namespace
