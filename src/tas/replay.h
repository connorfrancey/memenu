#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Mirror's Edge Replay (.merp): a recorded run. How it starts, the time step of every frame, every input the engine got,
// and the player's state on every frame so a playback can tell exactly where it stopped matching.
//
// The file is laid out like Geometry Dash's .gdr: a metadata header (author, description, the tool and game version
// that made it, level, frame rate, seed, duration), then the inputs as a sparse list of events, each with its frame
// number. It is chunked ("MERP" magic, then tagged chunks, then a checksum), so newer versions can add chunks that
// older readers skip, and anyone can add their own data in the extension chunk. ExportJson() writes the same content
// as readable JSON, like .gdr.json.
//
// Inputs are stored the way the engine receives them (UGameViewportClient::InputKey / InputAxis with the key's name),
// like mtas does. The game maps names to actions with your bindings; RenameKey() rewrites a replay for other bindings.
//
// In memory everything is flat (frames, then all events in frame order) so playing and recording walk two arrays
// sequentially.
namespace tas {

constexpr const char* kFileExtension = ".merp";

enum class StartKind : std::uint8_t {
    Now = 0,            // from wherever the player is: a macro. Can't be verified, the world isn't the same
    Command = 1,        // a console command that loads a level ("open edge_p")
    Checkpoint = 2,     // the level select's map + checkpoint (levels::Load with the Game method)
};

enum class EventKind : std::uint8_t {
    Pressed = 0, Released = 1, Repeat = 2, DoubleClick = 3,   // EInputEvent, InputKey
    Axis = 255,                                               // InputAxis
};

struct Event {
    std::uint16_t name = 0;          // index into Replay::names
    EventKind kind = EventKind::Pressed;
    std::uint8_t gamepad = 0;
    float value = 0.f;               // keys: amount depressed. Axis: delta
    float value2 = 0.f;              // Axis: delta time the engine passed along
};
static_assert(sizeof(Event) == 12);

struct Frame {
    float dt = 0.f;                  // seconds this frame advanced the game by
    std::uint32_t first = 0;         // first event in Replay::events
    std::uint32_t count = 0;         // number of events
};
static_assert(sizeof(Frame) == 12);

// The player at the start of a frame (before it ran). Compared bit for bit on playback.
struct Sync {
    float x = 0.f, y = 0.f, z = 0.f;
    float vx = 0.f, vy = 0.f, vz = 0.f;
    std::uint8_t state = 0;          // EMovement
    std::uint8_t present = 0;        // 0: there was no player on this frame
    std::uint16_t reserved = 0;

    bool operator==(const Sync& o) const;
};
static_assert(sizeof(Sync) == 28);

struct Replay {
    // ---- how it starts and runs ---------------------------------------------------------------------------------------
    StartKind start = StartKind::Now;
    std::string command;             // StartKind::Command
    std::string map, checkpoint;     // StartKind::Checkpoint: labels as the level select shows them ("" = level start)
    float fixedHz = 60.f;            // the frame rate it was recorded at; 0 = real time (every frame has its own dt)
    std::uint32_t seed = 0x4D455250; // CRT rand() seed on the game thread at the start

    // ---- metadata (like .gdr) -------------------------------------------------------------------------------------------
    std::string author, description;
    std::string botName, botVersion; // the tool that made it (set on save)
    std::uint32_t gameStamp = 0;     // PE timestamp of the MirrorsEdge.exe it was made with: the game version
    std::uint32_t rerecords = 0;     // times recording went back and continued from an earlier frame (TAS statistic)
    std::uint64_t created = 0;       // unix time
    std::map<std::string, std::string> extensions;    // free key/value data, kept as is

    // ---- data ----------------------------------------------------------------------------------------------------------
    std::vector<std::string> names;  // key/axis names ("W", "SpaceBar", "MouseX", ...)
    std::vector<Frame> frames;
    std::vector<Event> events;
    std::vector<Sync> sync;          // one per frame, or empty
    std::uint32_t syncValidUntil = 0;   // frames >= this were edited after recording: their sync is not checked

    void Clear();
    std::size_t FrameCount() const { return frames.size(); }
    double Duration() const;         // sum of dt (game seconds)

    std::uint16_t NameIndex(const std::string& name);   // adds it if new
    const std::string& Name(std::uint16_t i) const;

    // Recording. Events always go to the last frame.
    void BeginFrame(float dt);
    void AddEvent(const Event& e);
    // Drops every frame from `frame` on (keeping frames [0, frame)). Used to continue recording from there.
    void Truncate(std::size_t frame);

    // Editing (all keep `first` consistent and mark the sync after the change as unchecked).
    void InsertFrames(std::size_t at, std::size_t count, float dt);
    void EraseFrames(std::size_t at, std::size_t count);
    void InsertEvent(std::size_t frame, std::size_t indexInFrame, const Event& e);
    void EraseEvent(std::size_t frame, std::size_t indexInFrame);
    void ReplaceEvent(std::size_t frame, std::size_t indexInFrame, const Event& e);
    void SetFrameDt(std::size_t frame, float dt);
    void InvalidateSyncFrom(std::size_t frame);
    // Every event using `from` uses `to` instead. Returns how many changed.
    std::size_t RenameKey(const std::string& from, const std::string& to);

    // Identity of the inputs: FNV-1a over start, time steps and events. Two replays with the same id play the same.
    std::uint64_t InputHash() const;
    // Identity of the result: FNV-1a over the sync data.
    std::uint64_t SyncHash() const;

    bool Save(const std::filesystem::path& path, std::string* error) const;
    bool Load(const std::filesystem::path& path, std::string* error);
    bool ExportJson(const std::filesystem::path& path, std::string* error) const;
};

const char* KindName(EventKind kind);
const char* StartName(StartKind kind);
std::string Describe(const Replay& r, const Event& e);    // "W down", "MouseX +3.250"

} // namespace tas
