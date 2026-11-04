#include "speedrun/timer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace speedrun
{
// ---- Timer ---------------------------------------------------------------------------------------------------------
// Once started, the load removed time is counted the same way until Reset: also after Finish (it only freezes what is
// shown), so undoing the final split carries on as if the run never stopped, like LiveSplit does.

void Timer::Start(double now)
{
    phase_ = Phase::Running;
    startAt_ = now;
    lrtBefore_ = 0.0;
    since_ = now;
    endLrt_ = endRta_ = 0.0;
    splits_.clear();
}

void Timer::Reset()
{
    phase_ = Phase::Idle;
    lrtBefore_ = 0.0;
    endLrt_ = endRta_ = 0.0;
    splits_.clear();
}

void Timer::SetPaused(bool paused, double now)
{
    if (paused == paused_) return;
    paused_ = paused;
    if (phase_ == Phase::Idle) return;
    if (paused) lrtBefore_ += now - since_;
    else since_ = now;
}

double Timer::LiveLrt(double now) const
{
    if (phase_ == Phase::Idle) return 0.0;
    return lrtBefore_ + (paused_ ? 0.0 : std::max(0.0, now - since_));
}

double Timer::Lrt(double now) const
{
    return phase_ == Phase::Finished ? endLrt_ : LiveLrt(now);
}

double Timer::Rta(double now) const
{
    switch (phase_) {
        case Phase::Running:  return std::max(0.0, now - startAt_);
        case Phase::Finished: return endRta_;
        default:              return 0.0;
    }
}

void Timer::Split(std::string name, double now)
{
    if (phase_ != Phase::Running) return;
    splits_.push_back({std::move(name), LiveLrt(now)});
}

void Timer::Skip(std::string name)
{
    if (phase_ != Phase::Running) return;
    splits_.push_back({std::move(name), -1.0});
}

void Timer::Finish(std::string name, double now, int segments)
{
    if (phase_ != Phase::Running) return;
    while (segments > 0 && static_cast<int>(splits_.size()) + 1 < segments) splits_.push_back({std::string(), -1.0});
    endLrt_ = LiveLrt(now);
    endRta_ = std::max(0.0, now - startAt_);
    splits_.push_back({std::move(name), endLrt_});
    phase_ = Phase::Finished;
}

bool Timer::Undo()
{
    if (phase_ == Phase::Idle || splits_.empty()) return false;
    splits_.pop_back();
    phase_ = Phase::Running;
    return true;
}

double Timer::SegmentStart() const
{
    for (auto it = splits_.rbegin(); it != splits_.rend(); ++it)
        if (it->time >= 0.0) return it->time;
    return 0.0;
}

// ---- Comparison ----------------------------------------------------------------------------------------------------
double Comparison::PbTime() const
{
    return segments.empty() ? -1.0 : segments.back().pb;
}

double Comparison::SumOfBest() const
{
    if (segments.empty()) return -1.0;
    double sum = 0.0;
    for (const auto& s : segments) {
        if (s.best < 0.0) return -1.0;
        sum += s.best;
    }
    return sum;
}

double Comparison::SegmentTime(const std::vector<Split>& run, std::size_t i)
{
    if (i >= run.size() || run[i].time < 0.0) return -1.0;
    if (i == 0) return run[0].time;
    if (run[i - 1].time < 0.0) return -1.0;
    return run[i].time - run[i - 1].time;
}

void Comparison::EnsureSegments(const std::vector<std::string>& names)
{
    if (segments.size() < names.size()) segments.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        if (segments[i].name.empty()) segments[i].name = names[i];
}

int Comparison::UpdateBest(const std::vector<Split>& run)
{
    int improved = 0;
    for (std::size_t i = 0; i < run.size() && i < segments.size(); ++i) {
        const double t = SegmentTime(run, i);
        if (t >= 0.0 && (segments[i].best < 0.0 || t < segments[i].best)) {
            segments[i].best = t;
            ++improved;
        }
    }
    return improved;
}

bool Comparison::Finish(const std::vector<Split>& run)
{
    ++completed;
    if (run.size() > segments.size()) {
        std::vector<std::string> names;
        for (const auto& s : run) names.push_back(s.name);
        EnsureSegments(names);
    }
    UpdateBest(run);
    const bool complete = !run.empty() && run.size() == segments.size() && run.back().time >= 0.0;
    if (!complete) return false;
    const double pb = PbTime();
    if (pb >= 0.0 && run.back().time >= pb) return false;
    for (std::size_t i = 0; i < run.size(); ++i) segments[i].pb = run[i].time;
    return true;
}

void Comparison::ForgetPb()
{
    for (auto& s : segments) s.pb = s.best = -1.0;
}

namespace {
nlohmann::json Time(double t) { return t >= 0.0 ? nlohmann::json(t) : nlohmann::json(nullptr); }
double Time(const nlohmann::json& j) { return j.is_number() ? std::max(0.0, j.get<double>()) : -1.0; }
}

std::string Comparison::ToJson() const
{
    nlohmann::ordered_json j;
    j["category"] = category;
    j["attempts"] = attempts;
    j["completed"] = completed;
    auto& segs = j["segments"] = nlohmann::ordered_json::array();
    for (const auto& s : segments) segs.push_back({{"name", s.name}, {"pb", Time(s.pb)}, {"best", Time(s.best)}});
    return j.dump(2);
}

bool Comparison::FromJson(const std::string& text, Comparison& out)
{
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    Comparison c;
    c.category = j.value("category", std::string());
    c.attempts = std::max(0, j.value("attempts", 0));
    c.completed = std::max(0, j.value("completed", 0));
    if (const auto it = j.find("segments"); it != j.end() && it->is_array())
        for (const auto& s : *it) {
            if (!s.is_object()) continue;
            Segment seg;
            seg.name = s.value("name", std::string());
            seg.pb = s.contains("pb") ? Time(s["pb"]) : -1.0;
            seg.best = s.contains("best") ? Time(s["best"]) : -1.0;
            c.segments.push_back(std::move(seg));
        }
    out = std::move(c);
    return true;
}

// ---- Formatting ----------------------------------------------------------------------------------------------------
std::string Format(double seconds, int decimals)
{
    decimals = std::clamp(decimals, 0, 3);
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    static const long long kScale[] = {1, 10, 100, 1000};
    const long long scale = kScale[decimals];
    // Truncated, not rounded (a timer never shows a time that hasn't passed yet). The epsilon keeps 1.23 at 1.23.
    const long long units = static_cast<long long>(std::floor(seconds * static_cast<double>(scale) + 1e-6));
    const long long whole = units / scale, frac = units % scale;
    const long long h = whole / 3600, m = whole / 60 % 60, s = whole % 60;

    char buf[48];
    if (h > 0)      std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, s);
    else if (m > 0) std::snprintf(buf, sizeof buf, "%lld:%02lld", m, s);
    else            std::snprintf(buf, sizeof buf, "%lld", s);
    std::string out = buf;
    if (decimals > 0) {
        std::snprintf(buf, sizeof buf, ".%0*lld", decimals, frac);
        out += buf;
    }
    return out;
}

std::string FormatDelta(double seconds, int decimals)
{
    if (!std::isfinite(seconds)) return {};
    return (seconds < 0.0 ? "-" : "+") + Format(std::fabs(seconds), decimals);
}

std::string FileName(const std::string& category)
{
    std::string out = category;
    for (char& c : out)
        if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32) c = '-';
    return out.empty() ? std::string("run") : out;
}
}
