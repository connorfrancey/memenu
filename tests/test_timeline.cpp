// The TAS timeline (tas/timeline.h): holds from events and back, camera keys, and their place in the replay file.
#include "tas/macro.h"
#include "tas/timeline.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace tas;
using namespace tas::timeline;

namespace {

std::filesystem::path TempFile(const char* name) { return std::filesystem::temp_directory_path() / name; }

// W held 10..20, SpaceBar 15..17, mouse moving on 5 and 6; 40 frames.
Replay Sample()
{
    Macro m(60.f);
    m.Wait(5).Axis("MouseX", 1.f).Wait(1).Axis("MouseX", 2.f).Wait(4).Press("W").Wait(5).Hold("SpaceBar", 2).Wait(3)
        .Release("W").Wait(19);
    return m.Build();
}

std::uint16_t Name(Replay& r, const char* n) { return r.NameIndex(n); }

} // namespace

TEST(Timeline, HoldsFromEvents)
{
    Replay r = Sample();
    const auto w = Holds(r, Name(r, "W"));
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0].start, 10u);
    EXPECT_EQ(w[0].end, 20u);

    const auto tracks = Tracks(r);
    ASSERT_EQ(tracks.size(), 3u);               // SpaceBar, W (keys, by name), then MouseX
    EXPECT_EQ(r.Name(tracks[0].name), "SpaceBar");
    EXPECT_EQ(r.Name(tracks[1].name), "W");
    EXPECT_TRUE(tracks[2].axis);
    EXPECT_EQ(tracks[2].moves, (std::vector<std::uint32_t>{5, 6}));
}

TEST(Timeline, SetHoldsKeepsOtherInputs)
{
    Replay r = Sample();
    const auto before = r.events.size();
    const std::uint16_t w = Name(r, "W");
    SetHolds(r, w, {{2, 4}, {30, 35}});

    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{2, 4}, {30, 35}}));
    EXPECT_EQ(Holds(r, Name(r, "SpaceBar")), (std::vector<Hold>{{15, 17}}));
    EXPECT_EQ(Tracks(r).back().moves, (std::vector<std::uint32_t>{5, 6}));
    EXPECT_EQ(r.events.size(), before + 2);     // one more press and release

    // The frame table still points at the right events
    std::uint32_t first = 0;
    for (const Frame& f : r.frames) {
        EXPECT_EQ(f.first, first);
        first += f.count;
    }
    EXPECT_EQ(first, r.events.size());
}

TEST(Timeline, SetHoldsMergesAndGrows)
{
    Replay r = Sample();
    const std::uint16_t w = Name(r, "W");
    SetHolds(r, w, {{10, 20}, {18, 25}, {25, 27}, {5, 5}});   // overlap, touch, and an empty one
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{10, 27}}));

    SetHolds(r, w, {{38, 50}});                                // past the end: frames are added
    EXPECT_EQ(r.FrameCount(), 50u);
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{38, 50}}));     // released when the replay ends
}

TEST(Timeline, SetHoldsInvalidatesSyncFromTheChange)
{
    Replay r = Sample();
    r.sync.resize(r.FrameCount());
    r.syncValidUntil = static_cast<std::uint32_t>(r.FrameCount());
    SetHolds(r, Name(r, "W"), {{10, 22}});      // only the release moved, it was on frame 20
    EXPECT_EQ(r.syncValidUntil, 11u);           // the first changed hold starts on 10: frames after it are unchecked

    r.syncValidUntil = static_cast<std::uint32_t>(r.FrameCount());
    SetHolds(r, Name(r, "W"), {{10, 22}});      // nothing changed
    EXPECT_EQ(r.syncValidUntil, r.FrameCount());
}

TEST(Timeline, SetHeldOnOneFrame)
{
    Replay r = Sample();
    const std::uint16_t w = Name(r, "W");
    SetHeld(r, w, 15, false);                               // let go in the middle: the hold splits
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{10, 15}, {16, 20}}));
    SetHeld(r, w, 15, true);                                // and back: one hold again
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{10, 20}}));
    SetHeld(r, w, 20, true);                                // right after: grows it
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{10, 21}}));
    SetHeld(r, w, 30, true);                                // on its own: a one frame hold
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{10, 21}, {30, 31}}));
    SetHeld(r, w, 30, false);
    EXPECT_EQ(Holds(r, w), (std::vector<Hold>{{10, 21}}));
    EXPECT_TRUE(HeldAt(Holds(r, w), 10));
    EXPECT_FALSE(HeldAt(Holds(r, w), 21));
}

TEST(Timeline, SetAxisOnOneFrame)
{
    Replay r = Sample();
    const std::uint16_t x = Name(r, "MouseX");
    EXPECT_FLOAT_EQ(AxisAt(r, 6, x), 2.f);
    SetAxis(r, 6, x, -4.5f);
    EXPECT_FLOAT_EQ(AxisAt(r, 6, x), -4.5f);
    SetAxis(r, 8, x, 1.f);                                  // a frame that had none
    EXPECT_FLOAT_EQ(AxisAt(r, 8, x), 1.f);
    SetAxis(r, 5, x, 0.f);                                  // 0: the event goes
    EXPECT_FLOAT_EQ(AxisAt(r, 5, x), 0.f);
    EXPECT_EQ(Tracks(r).back().moves, (std::vector<std::uint32_t>{6, 8}));
    EXPECT_EQ(Holds(r, Name(r, "W")), (std::vector<Hold>{{10, 20}}));   // other inputs untouched
}

TEST(Timeline, CameraInterpolates)
{
    Replay r = Sample();
    float yaw = 0.f, pitch = 0.f;
    EXPECT_FALSE(CameraAt(r, 5, yaw, pitch));

    SetCameraKey(r, {10, 0.f, 0.f, Ease::Linear});
    SetCameraKey(r, {20, 90.f, -10.f, Ease::Smooth});
    SetCameraKey(r, {30, 0.f, 0.f, Ease::Hold});
    SetCameraKey(r, {35, 45.f, 0.f, Ease::Linear});

    EXPECT_FALSE(CameraAt(r, 9, yaw, pitch));
    ASSERT_TRUE(CameraAt(r, 15, yaw, pitch));
    EXPECT_FLOAT_EQ(yaw, 45.f);                 // linear, half way
    EXPECT_FLOAT_EQ(pitch, -5.f);
    ASSERT_TRUE(CameraAt(r, 22, yaw, pitch));   // smooth, 90 -> 0: leaves 90 slower than linear would (72)
    EXPECT_GT(yaw, 90.f - 0.2f * 90.f);
    EXPECT_LT(yaw, 90.f);
    ASSERT_TRUE(CameraAt(r, 33, yaw, pitch));   // hold: stays on the key
    EXPECT_FLOAT_EQ(yaw, 0.f);
    ASSERT_TRUE(CameraAt(r, 35, yaw, pitch));   // the last key itself
    EXPECT_FLOAT_EQ(yaw, 45.f);
    EXPECT_FALSE(CameraAt(r, 36, yaw, pitch));
}

TEST(Timeline, CameraKeysEditAndFollowFrames)
{
    Replay r = Sample();
    SetCameraKey(r, {10, 1.f, 0.f, Ease::Linear});
    SetCameraKey(r, {20, 2.f, 0.f, Ease::Linear});
    SetCameraKey(r, {20, 3.f, 0.f, Ease::Linear});        // replaces
    ASSERT_EQ(r.camera.size(), 2u);
    EXPECT_FLOAT_EQ(FindCameraKey(r, 20)->yaw, 3.f);

    EXPECT_TRUE(MoveCameraKey(r, 20, 5));
    EXPECT_EQ(r.camera.front().frame, 5u);                // still sorted
    EXPECT_FALSE(MoveCameraKey(r, 20, 6));

    r.InsertFrames(0, 3, 1.f / 60.f);                     // frames before them push them along
    EXPECT_EQ(r.camera[0].frame, 8u);
    EXPECT_EQ(r.camera[1].frame, 13u);
    r.EraseFrames(8, 2);                                  // the key on 8 goes with its frame
    ASSERT_EQ(r.camera.size(), 1u);
    EXPECT_EQ(r.camera[0].frame, 11u);
    r.Truncate(11);
    EXPECT_TRUE(r.camera.empty());
}

TEST(Timeline, CameraKeysSaveAndLoad)
{
    Replay r = Sample();
    SetCameraKey(r, {3, -20.f, 5.f, Ease::Hold});
    SetCameraKey(r, {12, 370.f, -30.f, Ease::Smooth});
    const auto withoutKeys = Sample().InputHash();
    EXPECT_NE(r.InputHash(), withoutKeys);                // the camera is an input

    std::string error;
    const auto merp = TempFile("timeline.merp");
    Replay back;
    ASSERT_TRUE(r.Save(merp, &error)) << error;
    ASSERT_TRUE(back.Load(merp, &error)) << error;
    std::filesystem::remove(merp);
    ASSERT_EQ(back.camera.size(), 2u);
    EXPECT_EQ(back.camera[1].frame, 12u);
    EXPECT_FLOAT_EQ(back.camera[1].yaw, 370.f);
    EXPECT_EQ(back.camera[0].ease, Ease::Hold);
    EXPECT_EQ(back.InputHash(), r.InputHash());

    const auto json = TempFile("timeline.json");
    Replay fromJson;
    ASSERT_TRUE(r.ExportJson(json, &error)) << error;
    ASSERT_TRUE(fromJson.ImportJson(json, &error)) << error;
    std::filesystem::remove(json);
    EXPECT_EQ(fromJson.InputHash(), r.InputHash());
}

TEST(Timeline, UnwrapTakesTheShortWay)
{
    EXPECT_FLOAT_EQ(UnwrapNear(350.f, 0.f), -10.f);
    EXPECT_FLOAT_EQ(UnwrapNear(10.f, 715.f), 730.f);
    EXPECT_FLOAT_EQ(UnwrapNear(-170.f, 170.f), 190.f);
}
