// Replays (.merp) and macros: everything of the TAS that runs without the game. The in-game part (playing a macro
// through the engine) is the "tas macro" step of the self-test, `memenu game-test`.
#include "tas/macro.h"
#include "tas/replay.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace tas;

namespace {

// Run forward for a second, jump half way, look a little to the right.
Replay RunAndJump()
{
    Macro m(60.f);
    m.Describe("run and jump")
        .Wait(5)
        .Press("W")
        .Wait(30)
        .Hold("SpaceBar", 2)
        .Axis("MouseX", 3.5f)
        .Wait(28)
        .Release("W");
    return m.Build();
}

// Events of `frame` as "Name kind" strings, in order.
std::vector<std::string> EventsAt(const Replay& r, std::size_t frame)
{
    std::vector<std::string> out;
    const Frame& f = r.frames.at(frame);
    for (std::uint32_t i = 0; i < f.count; ++i) {
        const Event& e = r.events.at(f.first + i);
        out.push_back(r.Name(e.name) + " " + KindName(e.kind));
    }
    return out;
}

// Every frame's `first` points right behind the previous frame's events: what playback walks.
void ExpectConsistent(const Replay& r)
{
    std::uint32_t next = 0;
    for (std::size_t i = 0; i < r.frames.size(); ++i) {
        EXPECT_EQ(r.frames[i].first, next) << "frame " << i;
        next += r.frames[i].count;
    }
    EXPECT_EQ(next, r.events.size());
}

std::filesystem::path TempFile(const char* name)
{
    return std::filesystem::temp_directory_path() / (std::string("memenu_test_") + name);
}

} // namespace

TEST(Macro, FramesAndEvents)
{
    const Replay r = RunAndJump();
    EXPECT_EQ(r.start, StartKind::Now);
    EXPECT_FLOAT_EQ(r.fixedHz, 60.f);
    EXPECT_EQ(r.description, "run and jump");
    ASSERT_EQ(r.FrameCount(), 66u);   // frames 0..65, nothing held at the end: no extra frame
    for (const Frame& f : r.frames) EXPECT_FLOAT_EQ(f.dt, 1.f / 60.f);
    EXPECT_NEAR(r.Duration(), 66.0 / 60.0, 1e-5);

    EXPECT_TRUE(EventsAt(r, 0).empty());
    EXPECT_EQ(EventsAt(r, 5), (std::vector<std::string>{"W " + std::string(KindName(EventKind::Pressed))}));
    EXPECT_EQ(EventsAt(r, 35), (std::vector<std::string>{"SpaceBar " + std::string(KindName(EventKind::Pressed))}));
    EXPECT_EQ(EventsAt(r, 37), (std::vector<std::string>{"SpaceBar " + std::string(KindName(EventKind::Released)),
                                                         "MouseX " + std::string(KindName(EventKind::Axis))}));
    EXPECT_EQ(EventsAt(r, 65), (std::vector<std::string>{"W " + std::string(KindName(EventKind::Released))}));
    EXPECT_EQ(r.names.size(), 3u);    // W, SpaceBar, MouseX: each name stored once
    ExpectConsistent(r);

    const Event& axis = r.events.at(r.frames[37].first + 1);
    EXPECT_FLOAT_EQ(axis.value, 3.5f);
    EXPECT_FLOAT_EQ(axis.value2, 1.f / 60.f);
}

TEST(Macro, ReleasesWhatIsStillHeld)
{
    Macro m;
    m.Press("W").Press("LeftShift").Wait(3).Release("LeftShift");
    const Replay r = m.Build();
    ASSERT_EQ(r.FrameCount(), 5u);    // 4 scripted frames + 1 that lets go of W
    EXPECT_EQ(EventsAt(r, 4), (std::vector<std::string>{"W " + std::string(KindName(EventKind::Released))}));
}

TEST(Macro, NoSyncToCheck)
{
    const Replay r = RunAndJump();
    EXPECT_TRUE(r.sync.empty());
    EXPECT_EQ(r.syncValidUntil, 0u);
}

TEST(Replay, SaveLoadRoundTrip)
{
    Replay r = RunAndJump();
    r.author = "tester";
    r.seed = 1234;
    r.extensions["note"] = "kept as is";
    const auto path = TempFile("roundtrip.merp");
    std::string error;
    ASSERT_TRUE(r.Save(path, &error)) << error;

    Replay back;
    ASSERT_TRUE(back.Load(path, &error)) << error;
    std::filesystem::remove(path);

    EXPECT_EQ(back.InputHash(), r.InputHash());
    EXPECT_EQ(back.start, r.start);
    EXPECT_EQ(back.seed, 1234u);
    EXPECT_EQ(back.author, "tester");
    EXPECT_EQ(back.description, "run and jump");
    EXPECT_EQ(back.extensions["note"], "kept as is");
    EXPECT_EQ(back.names, r.names);
    ASSERT_EQ(back.FrameCount(), r.FrameCount());
    ASSERT_EQ(back.events.size(), r.events.size());
    for (std::size_t i = 0; i < r.events.size(); ++i) {
        EXPECT_EQ(back.events[i].name, r.events[i].name);
        EXPECT_EQ(back.events[i].kind, r.events[i].kind);
        EXPECT_FLOAT_EQ(back.events[i].value, r.events[i].value);
    }
    ExpectConsistent(back);
}

TEST(Replay, RejectsDamagedFiles)
{
    const Replay r = RunAndJump();
    const auto path = TempFile("damaged.merp");
    std::string error;
    ASSERT_TRUE(r.Save(path, &error)) << error;
    {   // Flip one byte in the middle: the checksum must catch it
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(f.tellg());
        f.seekp(size / 2);
        char c = 0;
        f.seekg(size / 2);
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x5A);
        f.seekp(size / 2);
        f.write(&c, 1);
    }
    Replay back;
    EXPECT_FALSE(back.Load(path, &error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(path);

    EXPECT_FALSE(back.Load(TempFile("does_not_exist.merp"), &error));
}

TEST(Replay, InputHashFollowsInputs)
{
    const Replay a = RunAndJump(), b = RunAndJump();
    EXPECT_EQ(a.InputHash(), b.InputHash());   // Same inputs, same id

    Replay c = RunAndJump();
    c.SetFrameDt(10, 1.f / 30.f);
    EXPECT_NE(c.InputHash(), a.InputHash());

    Replay d = RunAndJump();
    EXPECT_EQ(d.RenameKey("W", "Z"), 2u);       // W pressed and released
    EXPECT_NE(d.InputHash(), a.InputHash());
    EXPECT_EQ(EventsAt(d, 5), (std::vector<std::string>{"Z " + std::string(KindName(EventKind::Pressed))}));
}

TEST(Replay, EditingKeepsFramesConsistent)
{
    Replay r = RunAndJump();
    const std::size_t events = r.events.size();

    r.InsertFrames(10, 4, 1.f / 60.f);
    ASSERT_EQ(r.FrameCount(), 70u);
    EXPECT_EQ(EventsAt(r, 39), (std::vector<std::string>{"SpaceBar " + std::string(KindName(EventKind::Pressed))}));
    ExpectConsistent(r);

    r.EraseFrames(36, 5);                       // Frames 36-40: the jump's press (now frame 39) goes with them
    ExpectConsistent(r);
    EXPECT_EQ(r.events.size(), events - 1);

    Event e;
    e.name = r.NameIndex("LeftShift");
    r.InsertEvent(0, 0, e);
    EXPECT_EQ(EventsAt(r, 0).size(), 1u);
    ExpectConsistent(r);
    r.EraseEvent(0, 0);
    EXPECT_TRUE(EventsAt(r, 0).empty());
    ExpectConsistent(r);

    r.Truncate(20);
    EXPECT_EQ(r.FrameCount(), 20u);
    ExpectConsistent(r);
}

TEST(Replay, ExportsJson)
{
    const Replay r = RunAndJump();
    const auto path = TempFile("export.merp.json");
    std::string error;
    ASSERT_TRUE(r.ExportJson(path, &error)) << error;
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);
    EXPECT_NE(text.find("SpaceBar"), std::string::npos);
    EXPECT_NE(text.find("run and jump"), std::string::npos);
}

TEST(Replay, JsonRoundTripKeepsInputs)
{
    const Replay r = RunAndJump();
    const auto path = TempFile("roundtrip.merp.json");
    std::string error;
    ASSERT_TRUE(r.ExportJson(path, &error)) << error;
    Replay back;
    const bool ok = back.ImportJson(path, &error);
    std::filesystem::remove(path);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(back.frames.size(), r.frames.size());
    EXPECT_EQ(back.events.size(), r.events.size());
    EXPECT_EQ(back.InputHash(), r.InputHash());
    EXPECT_EQ(back.description, "run and jump");
    ExpectConsistent(back);
}

TEST(Replay, ImportsHandWrittenJson)
{
    const auto path = TempFile("hand.json");
    {
        std::ofstream out(path);
        out << R"({"framerate": 60, "inputs": [
            {"frame": 10, "key": "SpaceBar", "event": "up"},
            {"frame": 0, "key": "W"},
            {"frame": 10, "key": "SpaceBar"}]})";
    }
    Replay r;
    std::string error;
    const bool ok = r.ImportJson(path, &error);
    std::filesystem::remove(path);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(r.start, StartKind::Now);
    EXPECT_EQ(r.frames.size(), 11u);
    EXPECT_EQ(EventsAt(r, 0), (std::vector<std::string>{"W down"}));
    EXPECT_EQ(EventsAt(r, 10), (std::vector<std::string>{"SpaceBar up", "SpaceBar down"}));   // file order kept
    ExpectConsistent(r);
}

// The words of the start kinds and camera eases are the enumerators' names (core/enum_names.h): renaming one would
// change the file format, so they are pinned here.
TEST(Replay, JsonWordsForStartAndEase)
{
    EXPECT_STREQ(StartName(StartKind::Now), "now");
    EXPECT_STREQ(StartName(StartKind::Command), "command");
    EXPECT_STREQ(StartName(StartKind::Checkpoint), "checkpoint");
    EXPECT_STREQ(EaseName(Ease::Linear), "linear");
    EXPECT_STREQ(EaseName(Ease::Smooth), "smooth");
    EXPECT_STREQ(EaseName(Ease::Hold), "hold");

    const auto path = TempFile("words.json");
    {
        std::ofstream out(path);
        out << R"json({"framerate": 60, "start": {"kind": "checkpoint", "map": "Prologue (edge_p)"},
            "inputs": [{"frame": 3, "key": "W"}],
            "camera": [{"frame": 0, "yaw": 0, "ease": "linear"}, {"frame": 1, "yaw": 10, "ease": "hold"},
                       {"frame": 2, "yaw": 20}]})json";
    }
    Replay r;
    std::string error;
    const bool ok = r.ImportJson(path, &error);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(r.start, StartKind::Checkpoint);
    ASSERT_EQ(r.camera.size(), 3u);
    EXPECT_EQ(r.camera[0].ease, Ease::Linear);
    EXPECT_EQ(r.camera[1].ease, Ease::Hold);
    EXPECT_EQ(r.camera[2].ease, Ease::Smooth);   // the default

    {
        std::ofstream out(path);
        out << R"({"start": {"kind": "Checkpoint"}, "inputs": []})";   // exact words only
    }
    EXPECT_FALSE(r.ImportJson(path, &error));
    std::filesystem::remove(path);
    EXPECT_NE(error.find("now, command or checkpoint"), std::string::npos) << error;
}

TEST(Replay, ImportRejectsBadJson)
{
    const auto path = TempFile("bad.json");
    {
        std::ofstream out(path);
        out << R"({"inputs": [{"frame": 1, "key": "W", "event": "sideways"}]})";
    }
    Replay r;
    std::string error;
    EXPECT_FALSE(r.ImportJson(path, &error));
    std::filesystem::remove(path);
    EXPECT_NE(error.find("sideways"), std::string::npos);
}

// The example macros in examples/replays/ load, survive a .merp round trip and leave no key pressed at the end.
TEST(Replay, ExamplesLoad)
{
    const std::filesystem::path folder = MEMENU_EXAMPLES_DIR "/replays";
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() != ".json") continue;
        SCOPED_TRACE(entry.path().filename().string());
        ++count;

        Replay r;
        std::string error;
        ASSERT_TRUE(r.ImportJson(entry.path(), &error)) << error;
        EXPECT_FALSE(r.description.empty());
        EXPECT_GT(r.FrameCount(), 0u);
        ExpectConsistent(r);

        std::vector<std::string> held;
        for (const Event& e : r.events) {
            const std::string& key = r.Name(e.name);
            if (e.kind == EventKind::Pressed) held.push_back(key);
            if (e.kind == EventKind::Released) held.erase(std::remove(held.begin(), held.end(), key), held.end());
        }
        EXPECT_TRUE(held.empty()) << "still pressed at the end: " << held.front();

        const auto path = TempFile("example.merp");
        Replay back;
        ASSERT_TRUE(r.Save(path, &error)) << error;
        ASSERT_TRUE(back.Load(path, &error)) << error;
        std::filesystem::remove(path);
        EXPECT_EQ(back.InputHash(), r.InputHash());
    }
    EXPECT_GT(count, 0u) << "no examples in " << folder.string();
}

TEST(Replay, MarkersSaveAndFollowEdits)
{
    Replay r = RunAndJump();
    r.SetMarker(30, "before the jump");
    r.SetMarker(10, "");                       // unnamed: "Frame 10"
    r.SetMarker(30, "jump");                   // the same frame: renamed
    ASSERT_EQ(r.markers.size(), 2u);
    EXPECT_EQ(r.markers[0].name, "Frame 10");  // sorted by frame
    EXPECT_EQ(r.markers[1].name, "jump");

    const auto merp = std::filesystem::temp_directory_path() / "memenu_markers.merp";
    const auto json = std::filesystem::temp_directory_path() / "memenu_markers.json";
    ASSERT_TRUE(r.Save(merp, nullptr));
    ASSERT_TRUE(r.ExportJson(json, nullptr));
    for (const auto& path : {merp, json}) {
        Replay back;
        ASSERT_TRUE(path.extension() == ".json" ? back.ImportJson(path, nullptr) : back.Load(path, nullptr));
        ASSERT_EQ(back.markers.size(), 2u) << path;
        EXPECT_EQ(back.markers[1].frame, 30u);
        EXPECT_EQ(back.markers[1].name, "jump");
        std::filesystem::remove(path);
    }
    EXPECT_EQ(r.InputHash(), RunAndJump().InputHash());   // Bookmarks don't change how it plays

    r.InsertFrames(5, 3, 1.f / 60.f);          // Both move 3 later
    EXPECT_EQ(r.markers[0].frame, 13u);
    EXPECT_EQ(r.markers[1].frame, 33u);
    r.EraseFrames(30, 5);                      // The jump's frame is cut: it moves to the cut
    EXPECT_EQ(r.markers[1].frame, 30u);
    r.Truncate(20);                            // Past the end: gone
    ASSERT_EQ(r.markers.size(), 1u);
    EXPECT_TRUE(r.EraseMarker(13));
    EXPECT_FALSE(r.EraseMarker(13));
}
