#pragma once
#include <string>
#include <vector>

// The speedrun timer's bookkeeping, without the game: the clock (load removed and real time), the splits of the run
// in progress and the comparison it runs against (personal best and best segments), saved as a small JSON file.
//
// Times are seconds. `now` is any monotonic clock in seconds (the game uses QueryPerformanceCounter), passed in so the
// logic can be tested without waiting.
namespace speedrun
{
    // One split of the run in progress: the load removed time it was reached at, or < 0 for a skipped one.
    struct Split
    {
        std::string name;
        double time = -1.0;
    };

    class Timer
    {
    public:
        enum class Phase { Idle, Running, Finished };

        Phase GetPhase() const { return phase_; }
        bool Running() const { return phase_ == Phase::Running; }
        bool Paused() const { return paused_; }

        void Start(double now);                     // From Idle (or Finished: a new run). Clears the splits.
        void Reset();                               // Back to Idle; the pause state is kept (a load may be going on)
        // Loads: while paused the load removed time stands still. Can be set in any phase.
        void SetPaused(bool paused, double now);

        double Lrt(double now) const;               // Load removed time
        double Rta(double now) const;               // Real time, loads included

        // Records a split at the current load removed time. Ignored unless running.
        void Split(std::string name, double now);
        void Skip(std::string name);                // A split without a time
        // The last split: stops the clock. Pads with skipped splits up to `segments` (when > 0) so the final time
        // lands on the last segment even if an auto split was missed.
        void Finish(std::string name, double now, int segments = 0);
        // Takes back the last split. Undoing the final one carries on with the run (the clock never stopped).
        bool Undo();

        const std::vector<speedrun::Split>& Splits() const { return splits_; }
        double FinalLrt() const { return endLrt_; }
        double FinalRta() const { return endRta_; }

        // Load removed time of the last split that has a time (0 at the start): the current segment began there.
        double SegmentStart() const;

    private:
        double LiveLrt(double now) const;

        Phase phase_ = Phase::Idle;
        bool paused_ = false;
        double startAt_ = 0.0;          // `now` at Start
        double lrtBefore_ = 0.0;        // Load removed time counted up to `since_`
        double since_ = 0.0;            // Last start / unpause
        double endLrt_ = 0.0, endRta_ = 0.0;
        std::vector<speedrun::Split> splits_;
    };

    // What a run is compared against, per category: one entry per split.
    struct Segment
    {
        std::string name;
        double pb = -1.0;       // Split time in the personal best (cumulative), < 0: none
        double best = -1.0;     // Fastest this segment was ever done, < 0: none
    };

    struct Comparison
    {
        std::string category;
        int attempts = 0;       // Runs started
        int completed = 0;      // Runs finished
        std::vector<Segment> segments;

        double PbTime() const;      // Final time of the personal best, < 0: none
        double SumOfBest() const;   // Sum of the best segments, < 0 unless every segment has one

        // Segment `i` of a run: from the previous split with a time to split `i`, if both have one and they're
        // adjacent (a skipped split in between makes it a combined segment: not a segment time). < 0: none.
        static double SegmentTime(const std::vector<Split>& run, std::size_t i);

        // Makes room for `count` segments (named from `names` where they have none yet).
        void EnsureSegments(const std::vector<std::string>& names);
        // Best segments beaten by `run` (also a run that was reset). Returns how many were improved.
        int UpdateBest(const std::vector<Split>& run);
        // A finished run: updates the best segments, and the personal best if it's complete (one split per segment, the
        // last with a time; skipped ones in between are fine) and faster. Returns true for a new personal best.
        bool Finish(const std::vector<Split>& run);
        void ForgetPb();            // Personal best and best segments

        std::string ToJson() const;
        static bool FromJson(const std::string& text, Comparison& out);
    };

    // "1:23.45", "1:02:03.4", "9.87". `decimals` 0..3. Negative times are shown as 0.
    std::string Format(double seconds, int decimals);
    // A difference to a comparison: "+1.2", "-12.34", "+1:02.3".
    std::string FormatDelta(double seconds, int decimals);
    // A name safe to use as a file name ("Any%" -> "Any%", "IL: Flight" -> "IL- Flight").
    std::string FileName(const std::string& category);
}
