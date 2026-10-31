#include "core/cache.h"
#include "core/clock.h"
#include "core/module.h"
#include "core/optimizations.h"
#include "core/scheduler.h"
#include "gui.h"

#include <Windows.h>
#include <psapi.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Memory {
    double workingSetMb = 0.0, privateMb = 0.0, peakMb = 0.0;
    unsigned long pageFaults = 0;
};

Memory ReadMemory()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    Memory m;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        m.workingSetMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m.privateMb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        m.peakMb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
        m.pageFaults = pmc.PageFaultCount;
    }
    return m;
}

// The performance monitor. Everything it shows is read from atomics or snapshots (taken at most 4 times a second), and
// its own history lives in fixed-size arrays: watching costs next to nothing.
class Performance final : public Module {
public:
    Performance() : Module("Performance", "Debug", "Frame times, scheduler, caches and memory", 5)
    {
        schedule.priority = sched::Priority::Background;
    }

    void OnRender() override
    {
        // Frame time of the render thread, from the menu's own clock.
        const clk::Ticks now = clk::Now();
        if (lastFrame_) {
            frameMs_[framePos_] = static_cast<float>(clk::Seconds(now - lastFrame_) * 1000.0);
            framePos_ = (framePos_ + 1) % frameMs_.size();
        }
        lastFrame_ = now;
        sched::SetBudget(budget);

        if (gui::visible || !compactWhenClosed) DrawWindow();
        else DrawCompact();
    }

private:
    static constexpr std::size_t kFrames = 240;

    struct Snapshot {
        std::vector<sched::EntryView> entries;
        std::vector<cache::StatsView> caches;
        sched::TickView ticks;
        std::vector<float> workMs, intervalMs;
    };

    const Snapshot& Data()
    {
        return snapshot_.Get([] {
            Snapshot s;
            s.entries = sched::Snapshot();
            s.caches = cache::Snapshot();
            s.ticks = sched::Ticks();
            sched::History(s.workMs, s.intervalMs);
            return s;
        });
    }

    // Average and worst of the last `kFrames` frames, in the order they happened.
    void FrameStats(float& avg, float& worst, std::array<float, kFrames>& ordered) const
    {
        float sum = 0.f;
        int n = 0;
        worst = 0.f;
        for (std::size_t i = 0; i < kFrames; ++i) {
            const float v = frameMs_[(framePos_ + i) % kFrames];
            ordered[i] = v;
            if (v <= 0.f) continue;
            sum += v;
            worst = std::max(worst, v);
            ++n;
        }
        avg = n ? sum / static_cast<float>(n) : 0.f;
    }

    void DrawCompact()
    {
        float avg, worst;
        std::array<float, kFrames> ordered;
        FrameStats(avg, worst, ordered);
        const auto t = sched::Ticks();
        char buf[160];
        std::snprintf(buf, sizeof buf, "%.0f fps  %.2f ms (worst %.1f)  menu %.2f ms/tick  x%.2f", avg > 0 ? 1000.f / avg : 0.f,
                      avg, worst, t.avgMs, t.pressure);
        const ImVec2 ds = ImGui::GetIO().DisplaySize, ts = ImGui::CalcTextSize(buf);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 pos{ds.x - ts.x - 12.f, ds.y - ts.y - 12.f};
        dl->AddRectFilled({pos.x - 4.f, pos.y - 2.f}, {pos.x + ts.x + 4.f, pos.y + ts.y + 2.f}, IM_COL32(0, 0, 0, 140));
        dl->AddText(pos, IM_COL32(255, 255, 255, 230), buf);
    }

    void DrawWindow()
    {
        ImGui::SetNextWindowSize({640.f, 480.f}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (!ImGui::Begin("Performance", &open)) {
            ImGui::End();
            if (!open) SetEnabled(false);
            return;
        }
        if (!open) SetEnabled(false);

        const Snapshot& d = Data();
        if (ImGui::BeginTabBar("perf")) {
            if (ImGui::BeginTabItem("Overview")) { DrawOverview(d); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Scheduler")) { DrawScheduler(d); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Caches")) { DrawCaches(d); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void DrawOverview(const Snapshot& d)
    {
        float avg, worst;
        std::array<float, kFrames> ordered;
        FrameStats(avg, worst, ordered);
        char label[96];

        std::snprintf(label, sizeof label, "Render: %.0f fps, %.2f ms avg, %.1f ms worst", avg > 0 ? 1000.f / avg : 0.f, avg, worst);
        ImGui::TextUnformatted(label);
        ImGui::PlotLines("##frames", ordered.data(), static_cast<int>(ordered.size()), 0, nullptr, 0.f,
                         std::max(33.f, worst), {-1.f, 60.f});

        float tickWorst = 0.f, tickSum = 0.f;
        int tickN = 0;
        for (float v : d.intervalMs) if (v > 0.f) { tickSum += v; tickWorst = std::max(tickWorst, v); ++tickN; }
        const float tickAvg = tickN ? tickSum / static_cast<float>(tickN) : 0.f;
        std::snprintf(label, sizeof label, "Game ticks: %.0f/s, %.2f ms apart, %.1f ms worst", tickAvg > 0 ? 1000.f / tickAvg : 0.f,
                      tickAvg, tickWorst);
        ImGui::TextUnformatted(label);
        if (!d.intervalMs.empty())
            ImGui::PlotLines("##ticks", d.intervalMs.data(), static_cast<int>(d.intervalMs.size()), 0, nullptr, 0.f,
                             std::max(33.f, tickWorst), {-1.f, 60.f});

        float workWorst = 0.f;
        for (float v : d.workMs) workWorst = std::max(workWorst, v);
        std::snprintf(label, sizeof label, "Menu work per tick: %.3f ms avg, %.3f ms last, budget %.2f ms",
                      d.ticks.avgMs, d.ticks.lastMs, d.ticks.budgetMs);
        ImGui::TextUnformatted(label);
        if (!d.workMs.empty())
            ImGui::PlotHistogram("##work", d.workMs.data(), static_cast<int>(d.workMs.size()), 0, nullptr, 0.f,
                                 std::max(d.ticks.budgetMs * 2.f, workWorst), {-1.f, 50.f});
        if (!opt::kEnabled)
            ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Built with no-optimizations: caches always recompute, "
                               "every module runs every tick");
        else if (d.ticks.pressure > 1.01)
            ImGui::TextColored({1.f, 0.75f, 0.3f, 1.f}, "Over budget: low priority intervals x%.2f, background x%.2f",
                               d.ticks.pressure, d.ticks.pressure * d.ticks.pressure);
        else
            ImGui::TextDisabled("Within budget (%llu ticks)", static_cast<unsigned long long>(d.ticks.ticks));

        ImGui::Separator();
        const Memory& m = memory_.Get(&ReadMemory);
        ImGui::Text("Memory: %.1f MB working set (peak %.1f), %.1f MB private, %lu page faults", m.workingSetMb, m.peakMb,
                    m.privateMb, m.pageFaults);

        // Most expensive modules at a glance
        std::vector<const sched::EntryView*> top;
        for (const auto& e : d.entries) top.push_back(&e);
        std::sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->avgUs + a->renderAvgUs > b->avgUs + b->renderAvgUs; });
        ImGui::Separator();
        ImGui::TextDisabled("Most expensive (tick + render, avg)");
        for (std::size_t i = 0; i < top.size() && i < 5; ++i)
            if (top[i]->avgUs + top[i]->renderAvgUs > 0.f)
                ImGui::Text("%-24s %8.1f us", top[i]->name.c_str(), top[i]->avgUs + top[i]->renderAvgUs);
    }

    void DrawScheduler(const Snapshot& d)
    {
        enum Col { Order, Name, Prio, State, Interval, Runs, Deferred, Avg, Max, RenderAvg, RenderMax, kCols };
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("sched", kCols, flags)) return;
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort, 0.f, Order);
        ImGui::TableSetupColumn("Name", 0, 0.f, Name);
        ImGui::TableSetupColumn("Priority", 0, 0.f, Prio);
        ImGui::TableSetupColumn("State", 0, 0.f, State);
        ImGui::TableSetupColumn("Every", 0, 0.f, Interval);
        ImGui::TableSetupColumn("Runs", 0, 0.f, Runs);
        ImGui::TableSetupColumn("Deferred", 0, 0.f, Deferred);
        ImGui::TableSetupColumn("Tick us", 0, 0.f, Avg);
        ImGui::TableSetupColumn("max", 0, 0.f, Max);
        ImGui::TableSetupColumn("Render us", 0, 0.f, RenderAvg);
        ImGui::TableSetupColumn("max", 0, 0.f, RenderMax);
        ImGui::TableHeadersRow();

        rows_.clear();
        for (const auto& e : d.entries) rows_.push_back(&e);
        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
            const auto col = specs->Specs[0].ColumnUserID;
            const bool asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            auto sortKey = [col](const sched::EntryView* e) -> double {
                switch (col) {
                    case Prio:      return static_cast<double>(e->priority);
                    case State:     return static_cast<double>(e->state);
                    case Interval:  return e->effectiveInterval;
                    case Runs:      return static_cast<double>(e->runs);
                    case Deferred:  return static_cast<double>(e->deferrals);
                    case Avg:       return e->avgUs;
                    case Max:       return e->maxUs;
                    case RenderAvg: return e->renderAvgUs;
                    case RenderMax: return e->renderMaxUs;
                    default:        return e->order;
                }
            };
            std::stable_sort(rows_.begin(), rows_.end(), [&](auto* a, auto* b) {
                if (col == Name) return asc ? a->name < b->name : a->name > b->name;
                return asc ? sortKey(a) < sortKey(b) : sortKey(a) > sortKey(b);
            });
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const sched::EntryView& e = *rows_[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", e.order);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
                if (!e.isModule) { ImGui::SameLine(); ImGui::TextDisabled("(task)"); }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(sched::PriorityName(e.priority));
                ImGui::TableNextColumn();
                if (e.state == sched::State::Deferred || e.state == sched::State::Failed)
                    ImGui::TextColored({1.f, 0.6f, 0.3f, 1.f}, "%s", sched::StateName(e.state));
                else
                    ImGui::TextUnformatted(sched::StateName(e.state));
                ImGui::TableNextColumn();
                if (e.interval > 0.f) ImGui::Text("%.2fs", e.effectiveInterval);
                else ImGui::TextDisabled("tick");
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(e.runs));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(e.deferrals));
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.avgUs);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.maxUs);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.renderAvgUs);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.renderMaxUs);
            }
        }
        ImGui::EndTable();
    }

    void DrawCaches(const Snapshot& d)
    {
        if (!ImGui::BeginTable("caches", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY))
            return;
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Cache", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Scope");
        ImGui::TableSetupColumn("Hits");
        ImGui::TableSetupColumn("Misses");
        ImGui::TableSetupColumn("Hit rate");
        ImGui::TableSetupColumn("Time computing");
        ImGui::TableHeadersRow();
        for (const auto& c : d.caches) {
            const auto total = c.hits + c.misses;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(cache::ScopeName(c.scope));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(c.hits));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(c.misses));
            ImGui::TableNextColumn();
            if (total) ImGui::Text("%.1f%%", 100.0 * static_cast<double>(c.hits) / static_cast<double>(total));
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.2f ms", c.computeSeconds * 1000.0);
        }
        ImGui::EndTable();
    }

    // Render thread state
    std::array<float, kFrames> frameMs_{};
    std::size_t framePos_ = 0;
    clk::Ticks lastFrame_ = 0;
    std::vector<const sched::EntryView*> rows_;          // reused: no allocation per frame once grown
    cache::Value<Snapshot> snapshot_{"Performance snapshot", cache::Scope::Timed, 0.25};
    cache::Value<Memory> memory_{"Process memory", cache::Scope::Timed, 0.5};

    ctl::Slider budget{this, "Tick budget", 1.5f, 0.1f, 8.f, "%.1f ms", 0.1f,
                       "Time per game tick for low priority work before the scheduler defers it and stretches intervals"};
    ctl::Checkbox compactWhenClosed{this, "Compact when menu closed", true,
                                    "Only a one-line summary in the corner while the menu is closed"};
};
MEM_MODULE(Performance)

} // namespace
