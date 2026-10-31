// Log viewer: an ImGui window with the lines the menu logged (the in-memory sink in log.cpp), filterable by level and
// text. While the menu is closed it can stay on screen as a click-through overlay.
#include "core/module.h"
#include "gui.h"
#include "log.h"
#include "theme.h"

#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kLevels = 6;
constexpr std::array<const char*, kLevels> kTags{"TRC", "DBG", "INF", "WRN", "ERR", "CRT"};

ImVec4 LevelColor(logging::Level l)
{
    switch (l) {
    case logging::Level::Trace:    return {0.50f, 0.50f, 0.55f, 1.f};
    case logging::Level::Debug:    return {0.45f, 0.65f, 0.85f, 1.f};
    case logging::Level::Info:     return theme::Menu().text;
    case logging::Level::Warn:     return {0.95f, 0.70f, 0.25f, 1.f};
    case logging::Level::Error:    return theme::Menu().accent;
    case logging::Level::Critical: return {1.00f, 0.35f, 0.35f, 1.f};
    }
    return theme::Menu().text;
}

class LogViewer final : public Module {
public:
    LogViewer() : Module("Log Viewer", "Debug", "Window with the menu's log, filterable by level and text", 50)
    {
        overlayAlpha.VisibleIf([this] { return overlay.Get(); });
        for (ctl::Checkbox* c : levels_) c->VisibleIf([] { return false; });   // drawn in the window's toolbar
    }

    void OnRender() override
    {
        Pull();

        const bool menu = gui::visible;
        if (!menu && !overlay) return;

        theme::Begin();
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
        if (!menu) {
            flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus;
            ImGui::SetNextWindowBgAlpha(overlayAlpha);
        }
        ImGui::SetNextWindowPos({24.f, 420.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({640.f, 300.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({320.f, 140.f}, {FLT_MAX, FLT_MAX});

        bool open = true;
        if (ImGui::Begin("Log Viewer", menu ? &open : nullptr, flags)) {
            if (menu) DrawToolbar();
            DrawLines(menu);
        }
        ImGui::End();
        theme::End();

        if (!open) SetEnabled(false);
    }

private:
    // ---- data ----------------------------------------------------------------------------------------------------------
    void Pull()
    {
        const std::size_t before = entries_.size();
        lastId_ = logging::CopySince(lastId_, entries_);
        if (entries_.size() == before) return;

        newLines_ = true;
        // Trim in chunks so a log at the limit doesn't force a full re-filter every frame.
        if (entries_.size() > logging::kMaxEntries + 500) {
            entries_.erase(entries_.begin(), entries_.end() - static_cast<std::ptrdiff_t>(logging::kMaxEntries));
            dirty_ = true;
            return;
        }
        if (!dirty_)
            for (std::size_t i = before; i < entries_.size(); ++i) Consider(i);
    }

    bool Passes(const logging::Entry& e) const
    {
        return levels_[static_cast<int>(e.level)]->Get() && filter_.PassFilter(e.text.c_str());
    }

    void Consider(std::size_t i)
    {
        ++counts_[static_cast<int>(entries_[i].level)];
        if (Passes(entries_[i])) shown_.push_back(static_cast<int>(i));
    }

    void Refilter()
    {
        shown_.clear();
        counts_.fill(0);
        for (std::size_t i = 0; i < entries_.size(); ++i) Consider(i);
        dirty_ = false;
    }

    void Clear()
    {
        entries_.clear();                      // lastId_ stays, so cleared lines don't come back
        dirty_ = true;
    }

    void CopyShown() const
    {
        std::string out;
        for (int i : shown_) {
            const logging::Entry& e = entries_[static_cast<std::size_t>(i)];
            out += e.time + " [" + kTags[static_cast<int>(e.level)] + "] " + e.text + '\n';
        }
        ImGui::SetClipboardText(out.c_str());
    }

    // ---- drawing -------------------------------------------------------------------------------------------------------
    void DrawToolbar()
    {
        for (int l = 0; l < kLevels; ++l) {
            ctl::Checkbox& on = *levels_[l];
            const ImVec4 c = LevelColor(static_cast<logging::Level>(l));
            ImGui::PushStyleColor(ImGuiCol_Text, on ? c : ImVec4{c.x, c.y, c.z, 0.35f});
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4{c.x, c.y, c.z, 0.18f} : ImVec4{0.f, 0.f, 0.f, 0.f});
            char label[32];
            std::snprintf(label, sizeof label, "%s %d##lvl%d", kTags[l], counts_[l], l);
            if (ImGui::SmallButton(label)) { on.Set(!on); dirty_ = true; }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Show/hide this level");
            ImGui::SameLine();
        }

        const float buttons = ImGui::CalcTextSize("Auto-scroll").x + ImGui::CalcTextSize("ClearCopyFile").x +
                              ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 6.f +
                              ImGui::GetStyle().ItemSpacing.x * 5.f;
        ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - buttons));
        if (ImGui::InputTextWithHint("##filter", "Filter (inc,-exc)", filter_.InputBuf, IM_ARRAYSIZE(filter_.InputBuf))) {
            filter_.Build();
            dirty_ = true;
        }

        ImGui::SameLine();
        bool follow = autoScroll;
        if (ImGui::Checkbox("Auto-scroll", &follow)) { autoScroll.Set(follow); scrollToEnd_ = follow; }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) Clear();
        ImGui::SameLine();
        if (ImGui::Button("Copy")) CopyShown();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Copy the shown lines");
        ImGui::SameLine();
        if (ImGui::Button("File")) OpenFile();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Open MirrorsEdgeMenu.log");
    }

    void DrawLines(bool menu)
    {
        if (dirty_) Refilter();

        ImGui::BeginChild("##lines", {0.f, 0.f}, menu ? ImGuiChildFlags_Borders : ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f, 1.f});

        if (shown_.empty())
            ImGui::TextDisabled(entries_.empty() ? "Nothing logged yet" : "No line matches the filter");

        const ImVec4 dim = theme::Menu().textDim;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const logging::Entry& e = entries_[static_cast<std::size_t>(shown_[static_cast<std::size_t>(row)])];
                const int l = static_cast<int>(e.level);
                ImGui::PushID(row);
                ImGui::TextColored(dim, "%s", e.time.c_str());
                ImGui::SameLine();
                ImGui::TextColored(LevelColor(e.level), "%s", kTags[l]);
                ImGui::SameLine();
                if (e.level >= logging::Level::Warn) ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(e.level));
                ImGui::TextUnformatted(e.text.c_str());
                if (e.level >= logging::Level::Warn) ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem("##line")) {
                    if (ImGui::MenuItem("Copy line")) ImGui::SetClipboardText(e.text.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        clipper.End();
        ImGui::PopStyleVar();

        // Follow new lines while scrolled to the bottom; scrolling up stops following until you return there.
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
        if (!menu || scrollToEnd_ || (autoScroll && newLines_ && atEnd)) ImGui::SetScrollHereY(1.f);
        scrollToEnd_ = false;
        newLines_ = false;
        ImGui::EndChild();
    }

    static void OpenFile()
    {
        const auto& file = logging::FilePath();
        if (!file.empty()) ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- state -----------------------------------------------------------------------------------------------------------
    std::vector<logging::Entry> entries_;
    std::vector<int> shown_;                   // indices into entries_ that pass the filter
    std::array<int, kLevels> counts_{};
    std::uint64_t lastId_ = 0;
    ImGuiTextFilter filter_;
    bool dirty_ = true, newLines_ = false, scrollToEnd_ = true;

    // ---- controls ------------------------------------------------------------------------------------------------------
    ctl::Checkbox overlay{this, "Show with menu closed", false, "Keep the window on screen (click-through) while the menu is closed"};
    ctl::Slider overlayAlpha{this, "Overlay opacity", 0.5f, 0.f, 1.f, "%.2f", 0.1f};
    ctl::Checkbox autoScroll{this, "Auto-scroll", true, "Follow new lines while scrolled to the bottom"};
    ctl::Button clear{this, "Clear", [this] { Clear(); }};
    ctl::Button openFile{this, "Open log file", [] { OpenFile(); }};

    // Level toggles: drawn in the window's toolbar, kept here so they are saved with the config.
    ctl::Checkbox showTrace{this, "Show trace", true};
    ctl::Checkbox showDebug{this, "Show debug", true};
    ctl::Checkbox showInfo {this, "Show info", true};
    ctl::Checkbox showWarn {this, "Show warn", true};
    ctl::Checkbox showError{this, "Show error", true};
    ctl::Checkbox showCrit {this, "Show critical", true};
    std::array<ctl::Checkbox*, kLevels> levels_{&showTrace, &showDebug, &showInfo, &showWarn, &showError, &showCrit};
};
MEM_MODULE(LogViewer)

} // namespace
