// Replays and macros: what a recording does every frame, and what saving, loading and the editor cost.
#include "bench.h"

#include "tas/macro.h"
#include "tas/replay.h"

#include <filesystem>
#include <string>

using namespace tas;

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

// Built once, on first use (registration stays cheap for --list)
const Replay& TenMinutes()
{
    static const Replay run = MakeRun(kTenMinutes);
    return run;
}
const Replay& OneMinute()
{
    static const Replay run = MakeRun(kOneMinute);
    return run;
}

std::filesystem::path TempFile(const char* name)
{
    return std::filesystem::temp_directory_path() / (std::string("memenu_bench_") + name);
}

// One recorded frame: what the game thread adds to every frame while recording
void RecordFrame(benchmark::State& state)
{
    Replay r;
    std::size_t f = 0;
    for (auto _ : state) {
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
    }
}
BENCHMARK(RecordFrame)->Name("Replay/record a frame (2 inputs)");

void InputHash(benchmark::State& state)
{
    const Replay& run = TenMinutes();
    for (auto _ : state) bench::Keep(run.InputHash());
    bench::PerItem(state, static_cast<double>(run.frames.size()), "frame");
}
BENCHMARK(InputHash)->Name("Replay/InputHash (10 min)")->Unit(benchmark::kMicrosecond);

void SyncHash(benchmark::State& state)
{
    const Replay& run = TenMinutes();
    for (auto _ : state) bench::Keep(run.SyncHash());
    bench::PerItem(state, static_cast<double>(run.frames.size()), "frame");
}
BENCHMARK(SyncHash)->Name("Replay/SyncHash (10 min)")->Unit(benchmark::kMicrosecond);

void CopyTruncate(benchmark::State& state)
{
    const Replay& run = TenMinutes();
    for (auto _ : state) {
        Replay r = run;
        r.Truncate(r.frames.size() - 120);
        bench::Keep(r.frames.size());
    }
    bench::PerItem(state, static_cast<double>(run.frames.size()), "frame");
}
BENCHMARK(CopyTruncate)->Name("Replay/copy + Truncate 2 s (practice rewind, 10 min)")->Unit(benchmark::kMicrosecond);

void SaveMerp(benchmark::State& state)
{
    const Replay& run = TenMinutes();
    const auto merp = TempFile("save.merp");
    std::string error;
    for (auto _ : state) bench::Keep(run.Save(merp, &error));
    bench::PerItem(state, static_cast<double>(run.frames.size()), "frame");
    std::error_code ec;
    std::filesystem::remove(merp, ec);
}
BENCHMARK(SaveMerp)->Name("Replay/Save .merp (10 min)")->Unit(benchmark::kMillisecond);

void LoadMerp(benchmark::State& state)
{
    const Replay& run = TenMinutes();
    const auto merp = TempFile("load.merp");
    std::string error;
    if (!run.Save(merp, &error)) {
        state.SkipWithError(("saving the replay to load failed: " + error).c_str());
        return;
    }
    for (auto _ : state) {
        Replay r;
        bench::Keep(r.Load(merp, &error));
    }
    bench::PerItem(state, static_cast<double>(run.frames.size()), "frame");
    std::error_code ec;
    std::filesystem::remove(merp, ec);
}
BENCHMARK(LoadMerp)->Name("Replay/Load .merp (10 min)")->Unit(benchmark::kMillisecond);

// JSON is for hand-edited macros: a minute is plenty (and ten take a minute per run in a debug build)
void ExportJson(benchmark::State& state)
{
    const Replay& minute = OneMinute();
    const auto json = TempFile("export.json");
    std::string error;
    for (auto _ : state) bench::Keep(minute.ExportJson(json, &error));
    bench::PerItem(state, static_cast<double>(minute.frames.size()), "frame");
    std::error_code ec;
    std::filesystem::remove(json, ec);
}
BENCHMARK(ExportJson)->Name("Replay/ExportJson (1 min)")->Unit(benchmark::kMillisecond);

void ImportJson(benchmark::State& state)
{
    const Replay& minute = OneMinute();
    const auto json = TempFile("import.json");
    std::string error;
    if (!minute.ExportJson(json, &error)) {
        state.SkipWithError(("exporting the JSON to import failed: " + error).c_str());
        return;
    }
    for (auto _ : state) {
        Replay r;
        bench::Keep(r.ImportJson(json, &error));
    }
    bench::PerItem(state, static_cast<double>(minute.frames.size()), "frame");
    std::error_code ec;
    std::filesystem::remove(json, ec);
}
BENCHMARK(ImportJson)->Name("Replay/ImportJson (1 min)")->Unit(benchmark::kMillisecond);

// Editor operations near the start of a long replay: every later frame's `first` moves
void InsertErase(benchmark::State& state)
{
    Replay r = TenMinutes();
    for (auto _ : state) {
        Event e;
        e.name = r.NameIndex("E");
        r.InsertEvent(10, 0, e);
        r.EraseEvent(10, 0);
    }
}
BENCHMARK(InsertErase)->Name("Replay/InsertEvent + EraseEvent at frame 10 (10 min)")->Unit(benchmark::kMicrosecond);

void RenameKey(benchmark::State& state)
{
    Replay r = TenMinutes();
    for (auto _ : state) {
        bench::Keep(r.RenameKey("W", "Up"));
        bench::Keep(r.RenameKey("Up", "W"));
    }
}
BENCHMARK(RenameKey)->Name("Replay/RenameKey W -> Up and back (10 min)")->Unit(benchmark::kMicrosecond);

void BuildMacro(benchmark::State& state)
{
    for (auto _ : state) {
        Macro m(kHz);
        m.Describe("bench");
        for (int i = 0; i < 60; ++i) m.Press("W").Wait(10).Axis("MouseX", 2.5f).Hold("SpaceBar", 5).Wait(15).Release("W");
        bench::Keep(m.Build().frames.size());
    }
}
BENCHMARK(BuildMacro)->Name("Macro/build a 1 minute macro")->Unit(benchmark::kMicrosecond);

// The shipped examples, as the menu reads them: one benchmark per file
const bool g_examples = [] {
    const std::filesystem::path folder = MEMENU_EXAMPLES_DIR "/replays";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".json") continue;
        const std::filesystem::path path = entry.path();
        benchmark::RegisterBenchmark("Macro/ImportJson " + path.filename().string(),
                                     [path](benchmark::State& state) {
                                         std::string error;
                                         for (auto _ : state) {
                                             Replay r;
                                             bench::Keep(r.ImportJson(path, &error));
                                         }
                                     })
            ->Unit(benchmark::kMicrosecond);
    }
    return true;
}();

} // namespace
