#include "tas/splits.h"

#include <algorithm>
#include <map>

namespace tas::splits {

bool IsSplit(const Marker& m) { return m.name.rfind(kSplitPrefix, 0) == 0; }

std::string SplitName(const std::string& level) { return kSplitPrefix + level; }

double TimeAt(const Replay& r, std::size_t frame)
{
    frame = std::min(frame, r.frames.size());
    double t = 0.0;
    for (std::size_t i = 0; i < frame; ++i) t += r.frames[i].dt;
    return t;
}

namespace {

// The rows of one replay, without the reference columns. One pass over the frames.
std::vector<Row> Rows(const Replay& r, bool splitsOnly)
{
    std::vector<Row> out;
    double t = 0.0, prev = 0.0;
    std::size_t f = 0;
    for (const Marker& m : r.markers) {        // Sorted by frame
        if (splitsOnly && !IsSplit(m)) continue;
        for (; f < m.frame && f < r.frames.size(); ++f) t += r.frames[f].dt;
        Row row;
        row.name = m.name;
        row.frame = m.frame;
        row.time = t;
        row.segment = t - prev;
        prev = t;
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace

Table Build(const Replay& run, const Replay* reference, bool splitsOnly)
{
    Table table;
    table.rows = Rows(run, splitsOnly);
    table.end = run.Duration();
    if (!reference) return table;
    table.refEnd = reference->Duration();

    // The n-th row of a name in the run gets the n-th one of that name in the reference.
    std::map<std::string, std::vector<const Row*>> byName;
    const std::vector<Row> ref = Rows(*reference, splitsOnly);
    for (const Row& r : ref) byName[r.name].push_back(&r);
    std::map<std::string, std::size_t> used;
    for (Row& row : table.rows) {
        const auto it = byName.find(row.name);
        if (it == byName.end()) continue;
        std::size_t& n = used[row.name];
        if (n >= it->second.size()) continue;
        row.refTime = it->second[n]->time;
        row.refSegment = it->second[n]->segment;
        ++n;
    }
    return table;
}

double Table::SumOfBest() const
{
    if (rows.empty()) return -1.0;
    double sum = 0.0;
    for (const Row& r : rows) {
        if (!r.HasRef()) return -1.0;
        sum += std::min(r.segment, r.refSegment);
    }
    return sum;
}

int Table::RowAt(std::size_t frame) const
{
    int found = -1;
    for (std::size_t i = 0; i < rows.size() && rows[i].frame <= frame; ++i) found = static_cast<int>(i);
    return found;
}

bool SetSplit(Replay& r, std::uint32_t frame, const std::string& name)
{
    if (frame >= r.frames.size()) return false;
    if (const Marker* here = r.MarkerAt(frame); here && (here->name == name || !IsSplit(*here)))
        return false;       // Already there, or one of the user's bookmarks is on that frame: it stays
    r.markers.erase(std::remove_if(r.markers.begin(), r.markers.end(),
                                   [&](const Marker& m) { return m.name == name && m.frame != frame; }),
                    r.markers.end());
    r.SetMarker(frame, name);
    return true;
}

} // namespace tas::splits
