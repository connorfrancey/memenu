// Replays and macros: what a recording does every frame, and what saving, loading and the editor cost.
#include "bench.h"

#include "tas/macro.h"
#include "tas/replay.h"

#include <filesystem>
#include <string>

using namespace tas;
namespace nb = ankerl::nanobench;

namespace {

constexpr float kHz = 60.f;
constexpr std::size_t kOneMinute = static_cast<std::size_t>(kHz) * 60;
constexpr std::size_t kTenMinutes = kOneMinute * 10;

// A run of `frames` frames with what a player typically does: a key change every few frames, mouse movement on most.
Replay MakeRun(std::size_t frames)
{
    Replay r;
    r.start = StartKind::Checkpoint;
    r.map = "Prologue (edge_p)";
    r.fixedHz = kHz;
    const char* keys[] = {"W", "A", "D", "SpaceBar", "LeftShift", "LeftMouseButton"};
    for (std::size_t f = 0; f < frames; ++f) {
        r.BeginFrame(1.f / kHz);
        if (f % 7 == 0) {
            Event e;
            e.name = r.NameIndex(keys[(f / 7) % 6]);
            e.kind = (f / 7) % 2 ? EventKind::Released : EventKind::Pressed;
            e.value = e.kind == EventKind::Pressed ? 1.f : 0.f;
            r.AddEvent(e);
        }
        if (f % 3 != 0) {
            Event e;
            e.name = r.NameIndex(f % 2 ? "MouseX" : "MouseY");
            e.kind = EventKind::Axis;
            e.value = static_cast<float>(f % 17) - 8.f;
            e.value2 = 1.f / kHz;
            r.AddEvent(e);
        }
        Sync s;
        s.present = 1;
        s.x = static_cast<float>(f);
        r.sync.push_back(s);
    }
    r.syncValidUntil = static_cast<std::uint32_t>(r.frames.size());
    return r;
}

std::filesystem::path TempFile(const char* name)
{
    return std::filesystem::temp_directory_path() / (std::string("memenu_bench_") + name);
}

} // namespace

MEMENU_BENCH(Replay)
{
    const Replay run = MakeRun(kTenMinutes);
    std::string error;

    // One recorded frame: what the game thread adds to every frame while recording
    {
        Replay r;
        std::size_t f = 0;
        // Nanoseconds each: many iterations per epoch, or the timer's resolution shows
        bench.batch(1).unit("frame").minEpochIterations(200000).run("record a frame (2 inputs)", [&] {
            r.BeginFrame(1.f / kHz);
            Event e;
            e.name = r.NameIndex(f % 2 ? "W" : "MouseX");
            e.kind = f % 2 ? EventKind::Pressed : EventKind::Axis;
            r.AddEvent(e);
            r.AddEvent(e);
            if (++f == kTenMinutes) {
                r.Clear();
                f = 0;
            }
        });
        bench.minEpochIterations(5);
    }

    bench.batch(run.frames.size()).unit("frame");
    bench.run("InputHash (10 min)", [&] { nb::doNotOptimizeAway(run.InputHash()); });
    bench.run("SyncHash (10 min)", [&] { nb::doNotOptimizeAway(run.SyncHash()); });
    bench.run("copy + Truncate 2 s (practice rewind, 10 min)", [&] {
        Replay r = run;
        r.Truncate(r.frames.size() - 120);
        nb::doNotOptimizeAway(r.frames.size());
    });

    const auto merp = TempFile("run.merp");
    const auto json = TempFile("run.json");
    bench.run("Save .merp (10 min)", [&] { nb::doNotOptimizeAway(run.Save(merp, &error)); });
    bench.run("Load .merp (10 min)", [&] {
        Replay r;
        nb::doNotOptimizeAway(r.Load(merp, &error));
    });
    // JSON is for hand-edited macros: a minute is plenty (and ten take a minute per run in a debug build)
    const Replay minute = MakeRun(kOneMinute);
    bench.batch(minute.frames.size());
    bench.run("ExportJson (1 min)", [&] { nb::doNotOptimizeAway(minute.ExportJson(json, &error)); });
    bench.run("ImportJson (1 min)", [&] {
        Replay r;
        nb::doNotOptimizeAway(r.ImportJson(json, &error));
    });
    std::error_code ec;
    std::filesystem::remove(merp, ec);
    std::filesystem::remove(json, ec);

    // Editor operations near the start of a long replay: every later frame's `first` moves
    bench.batch(1).unit("edit");
    {
        Replay r = run;
        bench.run("InsertEvent + EraseEvent at frame 10 (10 min)", [&] {
            Event e;
            e.name = r.NameIndex("E");
            r.InsertEvent(10, 0, e);
            r.EraseEvent(10, 0);
        });
        bench.run("RenameKey W -> Up and back (10 min)", [&] {
            nb::doNotOptimizeAway(r.RenameKey("W", "Up"));
            nb::doNotOptimizeAway(r.RenameKey("Up", "W"));
        });
    }
}

MEMENU_BENCH(Macro)
{
    bench.batch(1).unit("macro");
    bench.run("build a 1 minute macro", [&] {
        Macro m(kHz);
        m.Describe("bench");
        for (int i = 0; i < 60; ++i) m.Press("W").Wait(10).Axis("MouseX", 2.5f).Hold("SpaceBar", 5).Wait(15).Release("W");
        nb::doNotOptimizeAway(m.Build().frames.size());
    });

    // The shipped examples, as the menu reads them
    const std::filesystem::path folder = MEMENU_EXAMPLES_DIR "/replays";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".json") continue;
        const std::string name = "ImportJson " + entry.path().filename().string();
        std::string error;
        bench.run(name, [&] {
            Replay r;
            nb::doNotOptimizeAway(r.ImportJson(entry.path(), &error));
        });
    }
}
