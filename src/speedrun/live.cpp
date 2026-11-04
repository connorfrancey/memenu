#include "speedrun/live.h"

#include "core/keybinds.h"
#include "core/keys.h"
#include "core/notify.h"
#include "game/game.h"
#include "game/uobject.h"
#include "paths.h"

#include <Windows.h>
#include <spdlog/spdlog.h>
#include <ME_SDK.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <sstream>

namespace fs = std::filesystem;

namespace speedrun
{
namespace {

// The game's map packages in story order, and the chapter names the game shows. "stromdrain_p" is spelled that way
// in the game; the other spelling is accepted in case a version fixed it.
const struct { const char* map; const char* title; } kChapters[] = {
    {"edge_p", "Prologue"},   {"escape_p", "Flight"},   {"stromdrain_p", "Jacknife"}, {"cranes_p", "Heat"},
    {"subway_p", "Ropeburn"}, {"mall_p", "New Eden"},   {"factory_p", "Factory"},     {"boat_p", "The Boat"},
    {"convoy_p", "Kate"},     {"scraper_p", "The Shard"},
};
constexpr int kFinalChapter = 9;
constexpr const char* kMenuMap = "tdmainmenu";
constexpr const char* kLevelCompleted = "Function TdGame.SeqAct_TdLevelCompleted.Activated";
// A new game: the difficulty screen's accept. Accepting writes the profile, and the game starts once that completes;
// both are delegates the engine calls through ProcessEvent. Either one counts (whichever the game version calls).
constexpr const char* kNewGameEvents[] = {
    "Function TdGame.TdUIScene_DifficultySettings.OnProfileWriteComplete",
    "Function TdGame.TdUIScene_DifficultySettings.OnStartGame",
};
// Where Faith grabs the helicopter at the end of the Shard: the end of a story run. The spot and radius are the ones
// Mirror's Edge No Loads (fatalis' LiveSplit component, the community's timer) ends runs at.
constexpr float kHeliGrab[3] = {-6987.5f, 9672.7f, 75237.5f};
constexpr float kHeliGrabRadius = 12.f;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool IsStory(Category c) { return c != Category::IndividualLevel; }

// ---- State: game thread, under g_mutex (the menu reads it through Snapshot) -----------------------------------------
std::mutex g_mutex;
Options g_options;
Timer g_timer;
Comparison g_cmp;
std::optional<Comparison> g_cmpBeforeFinish;    // To undo a final split that set a personal best
std::string g_cmpKey;                           // File the comparison is from ("Any%", "IL - Flight")
std::vector<std::string> g_names;               // Segments of that category
bool g_newPb = false;
bool g_hooked = false;
std::string g_map;                              // Lower case
std::string g_lastEvent;
std::vector<std::function<void(double)>> g_commands;

// Game thread only
bool g_registered = false;
bool g_inLoad = false;          // Between the level load's start and end
bool g_mapPending = false;      // A level was loaded and its name not read yet (the world appears a tick or two later)
std::string g_completedHere;    // Map the last "level completed" came from, since entering it (the event can repeat)
bool g_newGameStart = false;    // A run was started by a new game in the main menu; the Prologue isn't loaded yet
bool g_newGameSeen = false;     // The new game event worked once: Prologue loads from the menu no longer start runs
int g_keyWas[4] = {};
unsigned g_ticks = 0;

fs::path FileFor(const std::string& key) { return paths::Dir() / "speedrun" / (FileName(key) + ".json"); }

void Save()
{
    if (g_cmpKey.empty()) return;
    std::error_code ec;
    fs::create_directories(FileFor(g_cmpKey).parent_path(), ec);
    std::ofstream out(FileFor(g_cmpKey), std::ios::binary | std::ios::trunc);
    if (out) out << g_cmp.ToJson();
    else spdlog::warn("Speedrun: could not save {}", FileFor(g_cmpKey).string());
}

// Which file a run of the current category in `map` compares against, and its segments.
std::string KeyFor(Category c, const std::string& map)
{
    if (IsStory(c)) return CategoryName(c);
    const std::string title = ChapterTitle(map);
    return "IL - " + (title.empty() ? map : title);
}

std::vector<std::string> NamesFor(Category c, const std::string& map)
{
    if (IsStory(c)) return Chapters();
    const std::string title = ChapterTitle(map);
    return {title.empty() ? map : title};
}

void Load(const std::string& key, std::vector<std::string> names)
{
    if (key == g_cmpKey) return;
    Comparison c;
    if (std::ifstream in(FileFor(key), std::ios::binary); in) {
        std::stringstream ss;
        ss << in.rdbuf();
        if (!Comparison::FromJson(ss.str(), c)) spdlog::warn("Speedrun: {} is not valid, starting over", FileFor(key).string());
    }
    c.category = key;
    c.EnsureSegments(names);
    g_cmp = std::move(c);
    g_cmpKey = key;
    g_names = std::move(names);
    g_cmpBeforeFinish.reset();
}

void Event(std::string text)
{
    spdlog::info("Speedrun: {}", text);
    g_lastEvent = std::move(text);
}

// A run that ends without finishing: its best segments still count.
void EndAttempt()
{
    if (g_timer.GetPhase() == Timer::Phase::Running && g_cmp.UpdateBest(g_timer.Splits()) > 0) Save();
    g_timer.Reset();
    g_newPb = false;
    g_cmpBeforeFinish.reset();
}

void BeginRun(double now)
{
    EndAttempt();
    Load(KeyFor(g_options.category, g_map), NamesFor(g_options.category, g_map));
    ++g_cmp.attempts;
    Save();
    g_timer.Start(now);
    Event("started " + g_cmpKey + " (attempt " + std::to_string(g_cmp.attempts) + ")");
}

void FinishRun(const std::string& name, double now)
{
    g_cmpBeforeFinish = g_cmp;
    g_timer.Finish(name, now, static_cast<int>(g_names.size()));
    g_newPb = g_cmp.Finish(g_timer.Splits());
    Save();
    Event("finished " + g_cmpKey + " in " + Format(g_timer.FinalLrt(), 3) + (g_newPb ? " (personal best)" : ""));
    if (g_newPb) notify::Push("Personal best! " + Format(g_timer.FinalLrt(), 2));
}

std::string NextName()
{
    const std::size_t i = g_timer.Splits().size();
    return i < g_names.size() ? g_names[i] : "Split " + std::to_string(i + 1);
}

void SplitOrFinish(double now)
{
    if (g_timer.Splits().size() + 1 >= g_names.size()) FinishRun(NextName(), now);
    else {
        const std::string name = NextName();
        g_timer.Split(name, now);
        Event("split " + name + " at " + Format(g_timer.Splits().back().time, 3));
    }
}

// ---- What the game does -------------------------------------------------------------------------------------------
void OnEnterLevel(const std::string& from, const std::string& map, double now)
{
    g_completedHere.clear();
    if (map == kMenuMap) g_newGameStart = false;     // Back in the menu: that new game never got to the Prologue
    if (map == kMenuMap || !g_options.enabled) return;
    const auto phase = g_timer.GetPhase();
    if (IsStory(g_options.category)) {
        const bool startedInMenu = std::exchange(g_newGameStart, false);
        // The run already started when the new game was accepted, or the new game event works and this is a Continue
        // or a chapter select: only the event starts runs then.
        if (ChapterIndex(map) != 0 || startedInMenu || g_newGameSeen) return;
        // Without the event: entering the Prologue from the main menu (a load of it in the middle of the Prologue is
        // a death or a checkpoint).
        const bool fromMenu = from.empty() || from == kMenuMap;
        if (fromMenu && (phase == Timer::Phase::Idle ? g_options.autoStart : g_options.autoReset)) BeginRun(now);
    } else {
        // Any load into a level starts it over: a restart, a checkpoint, another level.
        if (phase == Timer::Phase::Idle ? g_options.autoStart : g_options.autoReset) BeginRun(now);
    }
}

void OnLevelLoad(bool finished)
{
    std::lock_guard lock(g_mutex);
    g_inLoad = !finished;
    if (!finished) {
        if (g_options.removeLoads) g_timer.SetPaused(true, Now());
    } else {
        g_mapPending = true;       // Stays paused until the level is there
    }
}

void OnLevelCompleted(Classes::UObject* self, void* /*params*/)
{
    const double now = Now();
    std::string next;
    if (self && game::IsA(self, Classes::USeqAct_TdLevelCompleted::StaticClass())) {
        const auto& s = static_cast<Classes::USeqAct_TdLevelCompleted*>(self)->NextLevelName;
        if (s.IsValid()) next = s.ToString();
    }
    const std::string map = Lower(game::MapName());

    std::lock_guard lock(g_mutex);
    spdlog::info("Speedrun: level completed in {} (next: {})", map, next.empty() ? "none" : next);
    if (!g_options.enabled || map.empty() || map == g_completedHere) return;
    g_completedHere = map;
    if (!g_timer.Running()) return;

    if (!IsStory(g_options.category)) {
        if (g_options.autoFinish) FinishRun(g_names.empty() ? ChapterTitle(map) : g_names.back(), now);
        return;
    }
    const int chapter = ChapterIndex(map);
    if (chapter < 0) return;
    if (chapter == kFinalChapter) return;     // Story runs end at the helicopter grab (CheckHeliGrab)
    if (!g_options.autoSplit || static_cast<int>(g_timer.Splits().size()) > chapter) return;   // Already split
    while (static_cast<int>(g_timer.Splits().size()) < chapter) g_timer.Skip(g_names[g_timer.Splits().size()]);
    g_timer.Split(g_names[chapter], now);
    Event("split " + g_names[chapter] + " at " + Format(g_timer.Splits().back().time, 3));
}

void OnNewGame(Classes::UObject* /*self*/, void* /*params*/)
{
    const double now = Now();
    std::lock_guard lock(g_mutex);
    if (!g_options.enabled || !IsStory(g_options.category) || g_map != kMenuMap) return;
    if (g_newGameStart) return;                 // The second of the two events of one new game
    spdlog::info("Speedrun: new game");
    g_newGameSeen = true;
    const auto phase = g_timer.GetPhase();
    if (phase == Timer::Phase::Idle ? g_options.autoStart : g_options.autoReset) {
        BeginRun(now);
        g_newGameStart = true;
    }
}

// Game thread, under g_mutex. Story runs end when Faith grabs the helicopter at the end of the Shard.
void CheckHeliGrab(double now)
{
    if (!g_options.autoFinish || !IsStory(g_options.category) || !g_timer.Running()) return;
    if (ChapterIndex(g_map) != kFinalChapter) return;
    const auto* pawn = game::Pawn();
    if (!pawn) return;
    const float dx = pawn->Location.X - kHeliGrab[0], dy = pawn->Location.Y - kHeliGrab[1],
                dz = pawn->Location.Z - kHeliGrab[2];
    if (dx * dx + dy * dy + dz * dz > kHeliGrabRadius * kHeliGrabRadius) return;
    while (g_timer.Splits().size() + 1 < g_names.size()) g_timer.Skip(g_names[g_timer.Splits().size()]);
    FinishRun(ChapterTitle(g_map), now);
}

// The game is paused (its pause menu) or in the main menu: the clock stands still. Not right after a new game was
// accepted in the menu: the run is on from there.
bool InMenuOrPaused()
{
    if (g_map == kMenuMap && !g_newGameStart) return true;
    const auto* world = game::World();
    return world && world->Pauser;
}

void Register()
{
    if (g_registered) return;
    g_registered = true;
    game::OnLevelLoad(&OnLevelLoad);
    if (!game::OnScriptEvent(kLevelCompleted, &OnLevelCompleted))
        spdlog::warn("Speedrun: no auto splits (the level completed event can't be watched)");
    for (const char* e : kNewGameEvents)
        if (!game::OnScriptEvent(e, &OnNewGame)) spdlog::warn("Speedrun: {} can't be watched", e);
}

void PollKeys(const Options& o, double now)
{
    const int vk[4] = {o.keySplit, o.keyReset, o.keyUndo, o.keySkip};
    for (int i = 0; i < 4; ++i) {
        const bool down = vk[i] && keys::Down(vk[i]) && !keybinds::Armed();
        const bool pressed = down && !g_keyWas[i];
        g_keyWas[i] = down;
        if (!pressed) continue;
        switch (i) {
            case 0: StartOrSplit(); break;
            case 1: Reset(); break;
            case 2: Undo(); break;
            default: SkipSplit(); break;
        }
    }
    (void)now;
}

} // namespace

// ---- Public -----------------------------------------------------------------------------------------------------------
const char* CategoryName(Category c)
{
    switch (c) {
        case Category::AnyPercent:      return "Any%";
        case Category::Glitchless:      return "Glitchless";
        case Category::HundredPercent:  return "100%";
        default:                        return "Individual level";
    }
}

const std::vector<std::string>& CategoryNames()
{
    static const std::vector<std::string> names = {"Any%", "Glitchless", "100%", "Individual level"};
    return names;
}

const std::vector<std::string>& Chapters()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& c : kChapters) v.push_back(c.title);
        return v;
    }();
    return names;
}

int ChapterIndex(const std::string& map)
{
    const std::string m = Lower(map);
    if (m == "stormdrain_p") return 2;
    for (int i = 0; i < static_cast<int>(std::size(kChapters)); ++i)
        if (m == kChapters[i].map) return i;
    return -1;
}

std::string ChapterTitle(const std::string& map)
{
    const int i = ChapterIndex(map);
    return i < 0 ? std::string() : kChapters[i].title;
}

double Now()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

void SetOptions(const Options& o)
{
    std::lock_guard lock(g_mutex);
    const bool wasOn = g_options.enabled;
    g_options = o;
    if (!o.removeLoads && !o.pauseInMenus) g_timer.SetPaused(false, Now());
    if (wasOn && !o.enabled) EndAttempt();       // Switched off: a run in progress is over
}

void Tick()
{
    const double now = Now();
    Options o;
    {
        std::lock_guard lock(g_mutex);
        o = g_options;
    }
    if (!o.enabled) return;
    Register();
    if (g_ticks++ % 30 == 0) {
        const bool hooked = game::ScriptEventsReady();
        std::lock_guard lock(g_mutex);
        g_hooked = hooked;
    }

    std::string map;
    const bool readMap = g_mapPending || g_map.empty();
    if (readMap) map = Lower(game::MapName());
    const bool blocking = game::BlockingLoad();

    std::vector<std::function<void(double)>> commands;
    {
        std::lock_guard lock(g_mutex);
        if (readMap && !map.empty()) {
            const std::string from = g_map;
            g_map = map;
            if (g_mapPending) {
                g_mapPending = false;
                OnEnterLevel(from, map, now);
            }
        }
        if (g_timer.GetPhase() == Timer::Phase::Idle) Load(KeyFor(o.category, g_map), NamesFor(o.category, g_map));
        CheckHeliGrab(now);
        const bool loading = o.removeLoads && (g_inLoad || g_mapPending || blocking);
        g_timer.SetPaused(loading || (o.pauseInMenus && InMenuOrPaused()), now);
        commands.swap(g_commands);
        for (auto& c : commands) c(now);
    }
    PollKeys(o, now);
}

View Snapshot()
{
    std::lock_guard lock(g_mutex);
    View v;
    v.timer = g_timer;
    v.comparison = g_cmp;
    v.categoryLabel = g_cmpKey.empty() ? CategoryName(g_options.category) : g_cmpKey;
    v.segmentNames = g_names;
    v.loading = g_timer.Paused();
    v.newPb = g_newPb;
    v.hooked = g_hooked;
    v.map = g_map;
    v.lastEvent = g_lastEvent;
    v.now = Now();
    return v;
}

namespace {
void Post(std::function<void(double)> fn)
{
    std::lock_guard lock(g_mutex);
    g_commands.push_back(std::move(fn));
}
}

void StartOrSplit()
{
    Post([](double now) {
        if (g_timer.Running()) SplitOrFinish(now);
        else BeginRun(now);
    });
}

void Reset()
{
    Post([](double) {
        if (g_timer.GetPhase() == Timer::Phase::Idle) return;
        EndAttempt();
        Event("reset");
    });
}

void Undo()
{
    Post([](double) {
        const bool wasFinished = g_timer.GetPhase() == Timer::Phase::Finished;
        if (!g_timer.Undo()) return;
        if (wasFinished && g_cmpBeforeFinish) {
            g_cmp = *g_cmpBeforeFinish;     // The run isn't finished after all: neither PB nor completed count
            g_cmpBeforeFinish.reset();
            g_newPb = false;
            Save();
        }
        Event("undid the last split");
    });
}

void SkipSplit()
{
    Post([](double) {
        if (!g_timer.Running() || g_timer.Splits().size() + 1 >= g_names.size()) return;   // The last one can't be skipped
        g_timer.Skip(NextName());
        Event("skipped a split");
    });
}

void ForgetPb()
{
    Post([](double) {
        g_cmp.ForgetPb();
        g_cmpBeforeFinish.reset();
        Save();
        Event("forgot the personal best of " + g_cmpKey);
    });
}
}
