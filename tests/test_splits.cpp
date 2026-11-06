// TAS split times (tas/splits.h): times from bookmarks, the comparison with a reference run, automatic splits.
#include "tas/macro.h"
#include "tas/splits.h"

#include <gtest/gtest.h>

using namespace tas;

namespace {

// `frames` frames of 1/60 s.
Replay Frames(std::size_t frames)
{
    Macro m(60.f);
    m.Wait(frames - 1);
    return m.Build();
}

constexpr double kFrame = 1.0 / 60.0;

} // namespace

TEST(Splits, TimeAtAddsUpTheFramesBefore)
{
    Replay r = Frames(120);
    EXPECT_DOUBLE_EQ(splits::TimeAt(r, 0), 0.0);
    EXPECT_NEAR(splits::TimeAt(r, 60), 1.0, 1e-5);
    EXPECT_NEAR(splits::TimeAt(r, 500), r.Duration(), 1e-9);   // Past the end: all of it
}

TEST(Splits, RowsFromBookmarks)
{
    Replay r = Frames(300);
    r.SetMarker(60, splits::SplitName("Prologue"));
    r.SetMarker(90, "my bookmark");
    r.SetMarker(180, splits::SplitName("Flight"));

    const splits::Table all = splits::Build(r, nullptr, false);
    ASSERT_EQ(all.rows.size(), 3u);
    EXPECT_NEAR(all.rows[1].time, 90 * kFrame, 1e-5);
    EXPECT_NEAR(all.rows[1].segment, 30 * kFrame, 1e-5);
    EXPECT_FALSE(all.rows[0].HasRef());
    EXPECT_NEAR(all.end, 300 * kFrame, 1e-5);

    const splits::Table only = splits::Build(r, nullptr, true);
    ASSERT_EQ(only.rows.size(), 2u);
    EXPECT_EQ(only.rows[1].name, "Split: Flight");
    EXPECT_NEAR(only.rows[1].segment, 120 * kFrame, 1e-5);   // From the previous split, not the bookmark
    EXPECT_EQ(only.SumOfBest(), -1.0);                       // No reference
}

TEST(Splits, ComparedWithAReferenceByName)
{
    Replay ref = Frames(300);
    ref.SetMarker(60, splits::SplitName("Prologue"));
    ref.SetMarker(200, splits::SplitName("Flight"));

    Replay run = Frames(280);
    run.SetMarker(70, splits::SplitName("Prologue"));          // 10 frames behind
    run.SetMarker(180, splits::SplitName("Flight"));           // 20 frames ahead, the segment 30 frames faster
    run.SetMarker(200, splits::SplitName("Jacknife"));         // Not in the reference

    const splits::Table t = splits::Build(run, &ref, true);
    ASSERT_EQ(t.rows.size(), 3u);
    EXPECT_NEAR(t.rows[0].Delta(), 10 * kFrame, 1e-5);
    EXPECT_NEAR(t.rows[1].Delta(), -20 * kFrame, 1e-5);
    EXPECT_NEAR(t.rows[1].SegmentDelta(), -30 * kFrame, 1e-5);
    EXPECT_FALSE(t.rows[2].HasRef());
    EXPECT_NEAR(t.refEnd - t.end, 20 * kFrame, 1e-5);
    EXPECT_EQ(t.SumOfBest(), -1.0);                           // A row without a reference

    // Both have both splits: the best of each segment (60 from the reference, 110 from the run)
    run.EraseMarker(200);
    const splits::Table both = splits::Build(run, &ref, true);
    EXPECT_NEAR(both.SumOfBest(), 170 * kFrame, 1e-5);
}

TEST(Splits, RepeatedNamesMatchInOrder)
{
    Replay ref = Frames(100);
    ref.SetMarker(10, "Lap");
    ref.SetMarker(30, "Lap");
    Replay run = Frames(100);
    run.SetMarker(12, "Lap");
    run.SetMarker(40, "Lap");
    run.SetMarker(60, "Lap");

    const splits::Table t = splits::Build(run, &ref, false);
    ASSERT_EQ(t.rows.size(), 3u);
    EXPECT_NEAR(t.rows[0].Delta(), 2 * kFrame, 1e-5);
    EXPECT_NEAR(t.rows[1].Delta(), 10 * kFrame, 1e-5);
    EXPECT_FALSE(t.rows[2].HasRef());
}

TEST(Splits, RowAtIsTheLastRowReached)
{
    Replay r = Frames(100);
    r.SetMarker(10, "a");
    r.SetMarker(50, "b");
    const splits::Table t = splits::Build(r, nullptr, false);
    EXPECT_EQ(t.RowAt(5), -1);
    EXPECT_EQ(t.RowAt(10), 0);
    EXPECT_EQ(t.RowAt(49), 0);
    EXPECT_EQ(t.RowAt(99), 1);
}

TEST(Splits, SetSplitMovesItAndKeepsUserBookmarks)
{
    Replay r = Frames(100);
    const std::string flight = splits::SplitName("Flight");
    EXPECT_TRUE(splits::SetSplit(r, 40, flight));
    EXPECT_FALSE(splits::SetSplit(r, 40, flight));            // Already there: no change (a verified playback)

    EXPECT_TRUE(splits::SetSplit(r, 35, flight));             // The run got faster: the split moves
    ASSERT_EQ(r.markers.size(), 1u);
    EXPECT_EQ(r.markers[0].frame, 35u);

    r.SetMarker(50, "before the jump");
    EXPECT_FALSE(splits::SetSplit(r, 50, splits::SplitName("Jacknife")));   // The user's bookmark stays
    EXPECT_EQ(r.MarkerAt(50)->name, "before the jump");

    EXPECT_FALSE(splits::SetSplit(r, 100, flight));           // Past the end
    EXPECT_EQ(r.markers[0].frame, 35u);
}
