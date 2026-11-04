// Undo / redo of the TAS editors (tas/history.h).
#include "tas/history.h"
#include "tas/macro.h"
#include "tas/timeline.h"

#include <gtest/gtest.h>

using namespace tas;

namespace {
Replay Sample()
{
    Macro m(60.f);
    m.Wait(5).Press("W").Wait(20).Release("W").Wait(10);
    return m.Build();
}
}

TEST(History, UndoRedo)
{
    Replay r = Sample();
    EditHistory h;
    h.Watch(r);
    const auto w = r.NameIndex("W");

    h.Push(r, "Move W");
    timeline::SetHolds(r, w, {{10, 15}});
    h.Watch(r);                                        // Our own edit: kept
    h.Push(r, "Bookmark");
    r.SetMarker(3, "here");
    h.Watch(r);
    EXPECT_EQ(h.Steps(), 2u);
    EXPECT_EQ(h.UndoName(), "Bookmark");

    ASSERT_TRUE(h.Undo(r));
    EXPECT_TRUE(r.markers.empty());
    ASSERT_TRUE(h.Undo(r));
    EXPECT_EQ(timeline::Holds(r, w).front().start, 5u);
    EXPECT_FALSE(h.Undo(r));
    EXPECT_EQ(h.RedoName(), "Move W");

    ASSERT_TRUE(h.Redo(r));
    EXPECT_EQ(timeline::Holds(r, w).front().start, 10u);
    h.Watch(r);                                        // An undo / redo isn't a foreign change
    EXPECT_TRUE(h.CanUndo());
    EXPECT_TRUE(h.CanRedo());

    h.Push(r, "Delete");                               // A new edit forgets what was undone
    EXPECT_FALSE(h.CanRedo());
}

TEST(History, ForgetsWhenTheReplayIsReplaced)
{
    Replay r = Sample();
    EditHistory h;
    h.Watch(r);
    h.Push(r, "Move");
    timeline::SetHolds(r, r.NameIndex("W"), {{1, 2}});
    h.Watch(r);
    h.Watch(r);                                        // Nothing changed since
    EXPECT_TRUE(h.CanUndo());

    r.InsertFrames(0, 30, 1.f / 60.f);                 // Not through the history (a recording, a load)
    h.Watch(r);
    EXPECT_FALSE(h.CanUndo());
}

TEST(History, StaysWithinItsBudget)
{
    Replay r = Sample();
    r.SetMarker(0, "edit -");
    const std::size_t one = EditHistory::Bytes(r);   // Every step the same size: one bookmark, renamed
    EditHistory h(one * 3 + one / 2, 100);
    for (int i = 0; i < 10; ++i) {
        h.Push(r, "edit " + std::to_string(i));
        r.SetMarker(0, "edit " + std::to_string(i));
        h.Watch(r);
    }
    EXPECT_EQ(h.Steps(), 3u);                          // The oldest went
    EXPECT_EQ(h.UndoName(), "edit 9");
}
