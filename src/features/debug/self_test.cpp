// Test (developer mode only): runs the in-game self-tests (selftest.cpp) from the menu, in the game as it is, and shows
// what happens in an output console at the bottom of its window, like an IDE's output panel.
//
//   top      Run all / Run selected / Stop, the run's progress, and the options (level, fast mode, ticks)
//   middle   the tests to pick (feature tests, then every module by tab) and the steps of the current or last run,
//            each one expandable to its problems and notes
//   bottom   the output console: every line the menu logged during the runs (a separator per run), filterable,
//            errors only, copy, clear. Drag the bar above it to resize it.
//
// The window is only drawn with the menu open: the self-test measures what the menu draws with it closed.
#include "core/devmode.h"
#include "core/module.h"
#include "gui.h"
#include "log.h"
#include "selftest.h"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kPrefix = "Self-test: ";

ImVec4 Green() { return {0.35f, 0.80f, 0.45f, 1.f}; }
ImVec4 Yellow() { return {0.95f, 0.70f, 0.25f, 1.f}; }
ImVec4 Red() { return {1.00f, 0.38f, 0.35f, 1.f}; }
ImVec4 Blue() { return {0.45f, 0.65f, 0.95f, 1.f}; }

std::string Clock(double seconds)
{
    char buf[16];
    const int s = static_cast<int>(seconds);
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

// A status dot at the start of the current line (drawn, not a glyph: the menu font may not have one)
void Dot(ImVec4 color, bool pulse = false)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (pulse) color.w = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.f));
    ImGui::GetWindowDrawList()->AddCircleFilled({at.x + h * 0.5f, at.y + h * 0.5f}, h * 0.28f,
                                                ImGui::ColorConvertFloat4ToU32(color));
    ImGui::Dummy({h, h});
    ImGui::SameLine();
}

// One line of the output console
struct Line {
    std::string time, text;
    logging::Level level = logging::Level::Info;
    bool separator = false;     // "Run N" between runs
};

class SelfTest final : public Module {
public:
    SelfTest() : Module("Test", "Debug", "In-game self-tests with an output console (developer mode)", 60) {}

    bool Hidden() const override { return !devmode::Enabled(); }

    void OnRender() override
    {
        const auto progress = selftest::Snapshot();
        Pull(progress);                  // The console keeps collecting while the window isn't drawn
        if (Hidden() || !gui::visible) return;

        theme::Begin();
        ImGui::SetNextWindowPos({240.f, 120.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({820.f, 620.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({560.f, 380.f}, {FLT_MAX, FLT_MAX});
        bool open = true;
        if (ImGui::Begin("Test", &open, ImGuiWindowFlags_NoCollapse)) {
            DrawToolbar(progress);
            DrawOptions(progress.running);
            const float bottom = std::clamp(consoleHeight_, 90.f, std::max(90.f, ImGui::GetContentRegionAvail().y - 140.f));
            const float middle = ImGui::GetContentRegionAvail().y - bottom - kSplitter;
            DrawMiddle(progress, middle);
            DrawSplitter();
            DrawConsole(bottom);
        }
        ImGui::End();
        theme::End();
        if (!open) SetEnabled(false);
    }

private:
    static constexpr float kSplitter = 7.f;

    // ---- console data --------------------------------------------------------------------------------------------------
    // Every log line from a run's start until it ends; a separator line starts each run
    void Pull(const selftest::Progress& p)
    {
        if (p.run != run_ && p.run != 0) {
            run_ = p.run;
            lastId_ = p.firstLogId;
            collecting_ = true;
            Line sep;
            sep.separator = true;
            sep.text = "Run " + std::to_string(p.run);
            lines_.push_back(std::move(sep));
            newLines_ = true;
        }
        if (!collecting_) return;
        std::vector<logging::Entry> entries;
        lastId_ = logging::CopySince(lastId_, entries);
        for (auto& e : entries) {
            std::string text = std::move(e.text);
            if (text.rfind(kPrefix, 0) == 0) text.erase(0, std::char_traits<char>::length(kPrefix));
            lines_.push_back({std::move(e.time), std::move(text), e.level, false});
            newLines_ = true;
        }
        if (!p.running) collecting_ = false;   // This pull took the run's last lines
        if (lines_.size() > logging::kMaxEntries * 2)
            lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - logging::kMaxEntries));
    }

    ImVec4 LineColor(const Line& l) const
    {
        if (l.level >= logging::Level::Error) return Red();
        if (l.level == logging::Level::Warn) return Yellow();
        const std::string& t = l.text;
        if (t.find(" FAILED") != std::string::npos) return Red();
        if (t.size() >= 3 && t.compare(t.size() - 3, 3, " ok") == 0) return Green();
        if (t.find("PASSED") != std::string::npos) return Green();
        if (t.size() >= 3 && t.compare(t.size() - 3, 3, "...") == 0) return Blue();
        return t.rfind("run ", 0) == 0 ? theme::Menu().textActive : theme::Menu().text;
    }

    bool Shown(const Line& l) const
    {
        if (l.separator) return true;
        if (errorsOnly_ && l.level < logging::Level::Warn && l.text.find("FAILED") == std::string::npos) return false;
        return filter_.PassFilter(l.text.c_str());
    }

    // ---- top -----------------------------------------------------------------------------------------------------------
    void DrawToolbar(const selftest::Progress& p)
    {
        ImGui::BeginDisabled(p.running);
        if (ImGui::Button("Run all")) StartRun({});
        ImGui::SetItemTooltip("Every test and module, like memenu game-test (the measurement tests only when picked)");
        ImGui::SameLine();
        ImGui::BeginDisabled(picked_.empty());
        char label[48];
        std::snprintf(label, sizeof label, "Run selected (%zu)", picked_.size());
        if (ImGui::Button(label)) StartRun(picked_);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!p.running);
        if (ImGui::Button("Stop")) selftest::Stop();
        ImGui::SetItemTooltip("End the run at its next wait; settings and the frame rate cap are put back");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        int passed = 0, failed = 0;
        const selftest::StepView* current = nullptr;
        for (const auto& s : p.steps) {
            if (s.state == selftest::StepView::State::Passed) ++passed;
            else if (s.state == selftest::StepView::State::Failed) ++failed;
            if (s.state == selftest::StepView::State::Running) current = &s;
        }
        if (p.run == 0) {
            ImGui::TextDisabled("No run yet");
        } else if (p.running) {
            Dot(Blue(), true);
            ImGui::Text("Run %d  %s", p.run, current ? current->name.c_str() : "starting");
        } else {
            Dot(p.stopped ? Yellow() : failed ? Red() : Green());
            ImGui::Text("Run %d %s", p.run, p.stopped ? "stopped" : failed ? "failed" : "passed");
        }
        if (p.run != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d passed, %d failed, %s", passed, failed, Clock(p.seconds).c_str());
            const int done = static_cast<int>(p.steps.size()) - (current ? 1 : 0);
            const float fraction = p.running ? std::min(0.99f, static_cast<float>(done) / std::max(1, p.planned)) : 1.f;
            char overlay[32];
            std::snprintf(overlay, sizeof overlay, "%d / %d", done, std::max(done, p.planned));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, failed ? Red() : p.running ? Blue() : Green());
            ImGui::ProgressBar(fraction, {-FLT_MIN, 4.f}, "");
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s steps", overlay);
        }
    }

    void DrawOptions(bool running)
    {
        ImGui::BeginDisabled(running);
        bool load = loadLevel;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Level");
        ImGui::SameLine();
        if (ImGui::RadioButton("Current", !load)) loadLevel.Set(false);
        ImGui::SetItemTooltip("Test where the player is now (a level must be loaded)");
        ImGui::SameLine();
        if (ImGui::RadioButton("Load Prologue", load)) loadLevel.Set(true);
        ImGui::SetItemTooltip("Load edge_p at checkpoint 2 first, like memenu game-test");
        ImGui::SameLine(0.f, 18.f);
        bool f = fast;
        if (ImGui::Checkbox("Fast mode", &f)) fast.Set(f);
        ImGui::SetItemTooltip("The 62 fps cap off while testing (put back afterwards): waits count frames and ticks");
        ImGui::SameLine(0.f, 18.f);
        int t = ticks;
        ImGui::SetNextItemWidth(110.f);
        if (ImGui::SliderInt("Ticks per module", &t, 2, 120)) ticks.Set(t);
        ImGui::EndDisabled();
        ImGui::Spacing();
    }

    void StartRun(std::set<std::string> only)
    {
        selftest::RunOptions o;
        o.only = std::move(only);
        o.loadLevel = loadLevel;
        o.ticks = static_cast<unsigned>(ticks.Get());
        o.fast = fast;
        if (!selftest::Start(std::move(o))) return;
        scrollConsole_ = true;
    }

    // ---- middle: tests to pick | steps of the run -----------------------------------------------------------------------
    void DrawMiddle(const selftest::Progress& p, float height)
    {
        const float left = std::max(200.f, ImGui::GetContentRegionAvail().x * 0.34f);
        ImGui::BeginChild("##tests", {left, height}, ImGuiChildFlags_Borders);
        DrawPicker(p.running);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##steps", {0.f, height}, ImGuiChildFlags_Borders);
        DrawSteps(p);
        ImGui::EndChild();
    }

    void DrawPicker(bool running)
    {
        if (features_.empty()) features_ = selftest::FeatureTests();
        ImGui::TextDisabled("Tests");
        ImGui::SameLine();
        if (ImGui::SmallButton("All")) PickAll(true);
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) PickAll(false);
        ImGui::Separator();
        ImGui::BeginDisabled(running);
        if (!features_.empty() && ImGui::TreeNodeEx("Feature tests", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& t : features_) {
                Pick(t.name, t.name);
                if (t.optIn) ImGui::SetItemTooltip("A measurement: only runs when picked");
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Modules", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto& tab : modules::Tabs()) {
                std::vector<Module*> mods;
                for (Module* m : tab->modules)
                    if (Testable(*m)) mods.push_back(m);
                if (mods.empty()) continue;
                if (ImGui::TreeNode(tab->name.c_str())) {
                    for (Module* m : mods) Pick(m->Name(), m->Name() + "##" + tab->name);
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    }

    // The developer tools themselves are never switched by a run (selftest.cpp skips them too)
    static bool Testable(const Module& m) { return m.Name() != devmode::kModuleName && m.Name() != "Test"; }

    void Pick(const std::string& name, const std::string& label)
    {
        bool on = picked_.count(name) != 0;
        if (ImGui::Checkbox(label.c_str(), &on)) {
            if (on) picked_.insert(name);
            else picked_.erase(name);
        }
    }

    void PickAll(bool on)
    {
        picked_.clear();
        if (!on) return;
        for (const auto& t : features_) if (!t.optIn) picked_.insert(t.name);
        modules::ForEach([&](modules::Tab&, Module& m) { if (Testable(m)) picked_.insert(m.Name()); });
    }

    void DrawSteps(const selftest::Progress& p)
    {
        ImGui::TextDisabled("Steps");
        ImGui::Separator();
        if (p.steps.empty()) {
            ImGui::TextDisabled(p.running ? "Starting..." : "Run tests to see their steps here");
            return;
        }
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##steplist", 2, kFlags)) return;
        ImGui::TableSetupScrollFreeze(0, 0);
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 56.f);
        for (std::size_t i = 0; i < p.steps.size(); ++i) {
            const auto& s = p.steps[i];
            using State = selftest::StepView::State;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            Dot(s.state == State::Running ? Blue() : s.state == State::Failed ? Red() : Green(), s.state == State::Running);
            const bool details = !s.problems.empty() || !s.notes.empty();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!details) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (s.state == State::Failed) ImGui::PushStyleColor(ImGuiCol_Text, Red());
            const bool open = ImGui::TreeNodeEx(s.name.c_str(), flags);
            if (s.state == State::Failed) ImGui::PopStyleColor();
            if (s.state == State::Running && p.running && followSteps_) ImGui::SetScrollHereY(1.f);   // Keep it in view
            ImGui::TableNextColumn();
            if (s.state != State::Running) ImGui::TextDisabled("%.1f s", s.seconds);
            if (details && open) {
                for (const auto& problem : s.problems) RowText(problem, Red());
                for (const auto& note : s.notes) RowText(note, theme::Menu().textDim);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // Follow the running step while scrolled to the bottom (scrolling up to read stops it)
        followSteps_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
        ImGui::EndTable();
    }

    static void RowText(const std::string& text, ImVec4 color)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        if (ImGui::BeginPopupContextItem(text.c_str())) {
            if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(text.c_str());
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
    }

    // ---- bottom: output console ------------------------------------------------------------------------------------------
    void DrawSplitter()
    {
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##split", {width, kSplitter});
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (active) consoleHeight_ -= ImGui::GetIO().MouseDelta.y;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float y = (a.y + b.y) * 0.5f;
        ImVec4 c = theme::Menu().textDim;
        c.w = active ? 0.9f : ImGui::IsItemHovered() ? 0.6f : 0.25f;
        ImGui::GetWindowDrawList()->AddLine({a.x, y}, {b.x, y}, ImGui::ColorConvertFloat4ToU32(c), 2.f);
    }

    void DrawConsole(float height)
    {
        ImGui::BeginChild("##output", {0.f, height}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("Output");
        ImGui::SameLine();
        const float buttons = ImGui::CalcTextSize("Errors onlyAuto-scrollClearCopy").x + ImGui::GetFrameHeight() * 2.f +
                              ImGui::GetStyle().FramePadding.x * 4.f + ImGui::GetStyle().ItemSpacing.x * 6.f + 30.f;
        ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - buttons));
        ImGui::InputTextWithHint("##filter", "Filter (inc,-exc)", filter_.InputBuf, IM_ARRAYSIZE(filter_.InputBuf));
        if (ImGui::IsItemEdited()) filter_.Build();
        ImGui::SameLine();
        ImGui::Checkbox("Errors only", &errorsOnly_);
        ImGui::SetItemTooltip("Warnings, errors and failed steps");
        ImGui::SameLine();
        if (ImGui::Checkbox("Auto-scroll", &autoScroll_) && autoScroll_) scrollConsole_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) lines_.clear();
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) Copy();
        ImGui::SetItemTooltip("Copy the shown lines");

        ImGui::BeginChild("##lines", {0.f, 0.f}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f, 1.f});
        shown_.clear();
        for (int i = 0; i < static_cast<int>(lines_.size()); ++i)
            if (Shown(lines_[static_cast<std::size_t>(i)])) shown_.push_back(i);
        if (shown_.empty()) ImGui::TextDisabled(lines_.empty() ? "Output of the runs shows here" : "No line matches");

        const ImVec4 dim = theme::Menu().textDim;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const Line& l = lines_[static_cast<std::size_t>(shown_[static_cast<std::size_t>(row)])];
                if (l.separator) {
                    ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ImGui::SeparatorText(l.text.c_str());
                    ImGui::PopStyleColor();
                    continue;
                }
                ImGui::TextColored(dim, "%s", l.time.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, LineColor(l));
                ImGui::TextUnformatted(l.text.c_str());
                ImGui::PopStyleColor();
                ImGui::PushID(row);
                if (ImGui::BeginPopupContextItem("##line")) {
                    if (ImGui::MenuItem("Copy line")) ImGui::SetClipboardText(l.text.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        clipper.End();
        ImGui::PopStyleVar();
        // Follows new lines while scrolled to the bottom; scrolling up stops following until you come back down
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
        if (scrollConsole_ || (autoScroll_ && newLines_ && atEnd)) ImGui::SetScrollHereY(1.f);
        scrollConsole_ = newLines_ = false;
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void Copy() const
    {
        std::string out;
        for (int i : shown_) {
            const Line& l = lines_[static_cast<std::size_t>(i)];
            out += l.separator ? "---- " + l.text + " ----\n" : l.time + "  " + l.text + '\n';
        }
        ImGui::SetClipboardText(out.c_str());
    }

    // ---- state -------------------------------------------------------------------------------------------------------------
    std::vector<selftest::TestInfo> features_;
    std::set<std::string> picked_;
    std::vector<Line> lines_;
    std::vector<int> shown_;
    std::uint64_t lastId_ = 0;
    int run_ = 0;
    bool collecting_ = false, newLines_ = false, scrollConsole_ = false;
    bool autoScroll_ = true, errorsOnly_ = false, followSteps_ = true;
    float consoleHeight_ = 190.f;
    ImGuiTextFilter filter_;

    // Saved with the config (also in the module's settings panel)
    ctl::Checkbox loadLevel{this, "Load Prologue first", false, "Load edge_p at checkpoint 2 before testing, like memenu game-test"};
    ctl::Checkbox fast{this, "Fast mode", true, "The 62 fps cap off while testing (put back afterwards)"};
    ctl::SliderInt ticks{this, "Ticks per module", 10, 2, 120};
};
MEM_MODULE(SelfTest)

} // namespace
