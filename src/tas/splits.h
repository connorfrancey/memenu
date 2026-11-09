#pragma once
#include "tas/replay.h"

#include <cstdint>
#include <string>
#include <vector>

// Split times of a TAS, for speedrunners: every bookmark (or only the automatic "Split: <level>" ones the session adds
// when a level is completed) as a split, with the game time it is reached at and the segment since the previous one,
// compared with another replay (an older version of the run, someone else's run) by name.
//
// The time of a bookmark on frame f is the game time when frame f starts: the time steps of frames [0, f) added up.
// No ImGui and no game here (unit tested); the table is drawn by the TAS module.
namespace tas::splits {

constexpr const char* kSplitPrefix = "Split: ";

bool IsSplit(const Marker& m);                      // One the session added on a level completed ("Split: Flight")
std::string SplitName(const std::string& level);    // "Split: " + level

// Game seconds from the start of the replay to the start of `frame` (past the end: the whole duration).
double TimeAt(const Replay& r, std::size_t frame);

struct Row {
    std::string name;
    std::uint32_t frame = 0;
    double time = 0.0;              // From the start
    double segment = 0.0;           // From the previous row (the start for the first)
    double refTime = -1.0;          // The reference's split of the same name, < 0: none
    double refSegment = -1.0;       // That split's segment in the reference, < 0: none

    bool HasRef() const { return refTime >= 0.0; }
    double Delta() const { return time - refTime; }            // < 0: ahead of the reference
    double SegmentDelta() const { return segment - refSegment; }
};

struct Table {
    std::vector<Row> rows;
    double end = 0.0;               // Duration of the run
    double refEnd = -1.0;           // Duration of the reference, < 0: none
    // The sum of the best segment of each split (this run's or the reference's, whichever is faster), < 0 unless
    // every row has a reference.
    double SumOfBest() const;
    // The last row reached by `frame` (row.frame <= frame), -1 if none.
    int RowAt(std::size_t frame) const;
};

// `splitsOnly`: the automatic splits only, else every bookmark. Reference rows are matched by name: the n-th split
// called "Split: Flight" in the run with the n-th one in the reference.
Table Build(const Replay& run, const Replay* reference, bool splitsOnly);

// Puts the split `name` on `frame` (the frame must exist). A split of the same name on another frame is moved: a
// playback of an edited run moves its splits to where they happen now. A bookmark of the user's own on `frame` is
// kept (and the split not set). Returns true if the replay changed.
bool SetSplit(Replay& r, std::uint32_t frame, const std::string& name);

} // namespace tas::splits
