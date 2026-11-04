// The speedrun timer's bookkeeping (speedrun/timer.h): load removal, splits, personal best and best segments.
#include "speedrun/timer.h"

#include <gtest/gtest.h>

using namespace speedrun;

TEST(SpeedrunTimer, LoadsAreRemoved)
{
    Timer t;
    t.Start(100.0);
    EXPECT_DOUBLE_EQ(t.Lrt(110.0), 10.0);
    t.SetPaused(true, 110.0);                   // a 5 s load
    EXPECT_DOUBLE_EQ(t.Lrt(115.0), 10.0);
    t.SetPaused(false, 115.0);
    EXPECT_DOUBLE_EQ(t.Lrt(120.0), 15.0);
    EXPECT_DOUBLE_EQ(t.Rta(120.0), 20.0);
}

TEST(SpeedrunTimer, StartDuringALoad)
{
    Timer t;
    t.SetPaused(true, 0.0);
    t.Start(1.0);
    EXPECT_DOUBLE_EQ(t.Lrt(4.0), 0.0);
    t.SetPaused(false, 4.0);
    EXPECT_DOUBLE_EQ(t.Lrt(6.0), 2.0);
    t.Reset();
    EXPECT_FALSE(t.Paused());
    EXPECT_DOUBLE_EQ(t.Lrt(10.0), 0.0);
}

TEST(SpeedrunTimer, SplitsFinishAndUndo)
{
    Timer t;
    t.Split("ignored", 0.0);                    // not running yet
    t.Start(0.0);
    t.Split("A", 10.0);
    t.Skip("B");
    EXPECT_DOUBLE_EQ(t.SegmentStart(), 10.0);  // the skipped split has no time
    t.Finish("C", 30.0);
    ASSERT_EQ(t.Splits().size(), 3u);
    EXPECT_EQ(t.GetPhase(), Timer::Phase::Finished);
    EXPECT_DOUBLE_EQ(t.Lrt(99.0), 30.0);        // frozen

    EXPECT_TRUE(t.Undo());                      // carries on: the clock never stopped
    EXPECT_TRUE(t.Running());
    EXPECT_DOUBLE_EQ(t.Lrt(40.0), 40.0);
}

TEST(SpeedrunTimer, FinishPadsMissedSplits)
{
    Timer t;
    t.Start(0.0);
    t.Split("A", 5.0);
    t.Finish("D", 20.0, 4);                     // B and C were never split
    ASSERT_EQ(t.Splits().size(), 4u);
    EXPECT_LT(t.Splits()[1].time, 0.0);
    EXPECT_LT(t.Splits()[2].time, 0.0);
    EXPECT_DOUBLE_EQ(t.Splits()[3].time, 20.0);
}

TEST(SpeedrunComparison, PersonalBestAndGolds)
{
    Comparison c;
    c.EnsureSegments({"A", "B", "C"});
    EXPECT_LT(c.PbTime(), 0.0);

    EXPECT_TRUE(c.Finish({{"A", 10.0}, {"B", 25.0}, {"C", 40.0}}));
    EXPECT_DOUBLE_EQ(c.PbTime(), 40.0);
    EXPECT_DOUBLE_EQ(c.SumOfBest(), 40.0);

    // Slower overall, but a faster B: a gold, no new PB
    EXPECT_FALSE(c.Finish({{"A", 12.0}, {"B", 22.0}, {"C", 45.0}}));
    EXPECT_DOUBLE_EQ(c.PbTime(), 40.0);
    EXPECT_DOUBLE_EQ(c.segments[1].best, 10.0);
    EXPECT_DOUBLE_EQ(c.SumOfBest(), 35.0);

    // A skipped split (a missed auto split) still makes a PB: the final time is what counts. The segment after the
    // skip isn't a segment time, so its best stays.
    EXPECT_TRUE(c.Finish({{"A", 9.0}, {"B", -1.0}, {"C", 30.0}}));
    EXPECT_DOUBLE_EQ(c.PbTime(), 30.0);
    EXPECT_LT(c.segments[1].pb, 0.0);
    EXPECT_DOUBLE_EQ(c.segments[0].best, 9.0);
    EXPECT_DOUBLE_EQ(c.segments[2].best, 15.0);
    EXPECT_EQ(c.completed, 3);

    // Not finished on the last split: no PB
    EXPECT_FALSE(c.Finish({{"A", 1.0}, {"B", 2.0}}));
    EXPECT_DOUBLE_EQ(c.PbTime(), 30.0);
}

TEST(SpeedrunComparison, JsonRoundTrip)
{
    Comparison c;
    c.category = "Any%";
    c.attempts = 7;
    c.EnsureSegments({"Prologue", "Flight"});
    c.segments[0].pb = 61.5;
    c.segments[0].best = 60.25;

    Comparison back;
    ASSERT_TRUE(Comparison::FromJson(c.ToJson(), back));
    EXPECT_EQ(back.category, "Any%");
    EXPECT_EQ(back.attempts, 7);
    ASSERT_EQ(back.segments.size(), 2u);
    EXPECT_EQ(back.segments[1].name, "Flight");
    EXPECT_DOUBLE_EQ(back.segments[0].pb, 61.5);
    EXPECT_DOUBLE_EQ(back.segments[0].best, 60.25);
    EXPECT_LT(back.segments[1].pb, 0.0);

    EXPECT_FALSE(Comparison::FromJson("not json", back));
}

TEST(SpeedrunFormat, Times)
{
    EXPECT_EQ(Format(9.876, 2), "9.87");                // truncated
    EXPECT_EQ(Format(1.23, 2), "1.23");
    EXPECT_EQ(Format(83.45, 2), "1:23.45");
    EXPECT_EQ(Format(3723.4, 1), "1:02:03.4");
    EXPECT_EQ(Format(59.999, 0), "59");
    EXPECT_EQ(Format(-3.0, 2), "0.00");
    EXPECT_EQ(FormatDelta(-1.25, 2), "-1.25");
    EXPECT_EQ(FormatDelta(62.3, 1), "+1:02.3");
    EXPECT_EQ(FileName("IL: Flight"), "IL- Flight");
}
