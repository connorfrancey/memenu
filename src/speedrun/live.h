#pragma once
#include "speedrun/timer.h"

#include <string>
#include <vector>

// The speedrun timer in the game: starts, splits and stops runs from what the game does, removes loads, and keeps the
// personal best of each category in memenu\speedrun\<category>.json.
//
//   start      Story categories: a new game (accepting the difficulty screen). If that event is never seen, entering
//              the Prologue from the main menu. Individual level: entering any level.
//   split      A chapter is completed (the game's "level completed" Kismet action, which also starts the next one).
//   finish     Story: Faith grabs the helicopter at the end of the Shard. Individual level: the level is completed.
//   loads      The clock stands still while a level loads and while the game blocks on streaming (loading icon).
//   menus      The clock stands still while the game is paused and in the main menu (pauseInMenus).
//   reset      Story: a new game while a run is going or finished. Individual level: re-entering the level (a checkpoint
//              load or restart), when auto reset is on.
//
// Everything runs on the game thread (Tick, from the module); the menu reads a copy (Snapshot) and posts commands.
namespace speedrun
{
    enum class Category { AnyPercent, Glitchless, HundredPercent, IndividualLevel };
    const char* CategoryName(Category c);
    const std::vector<std::string>& CategoryNames();

    struct Options
    {
        bool enabled = false;
        Category category = Category::AnyPercent;
        bool autoStart = true, autoSplit = true, autoFinish = true, autoReset = true;
        bool removeLoads = true;
        bool pauseInMenus = true;   // Stopped while the game is paused or in the main menu
        int keySplit = 0, keyReset = 0, keyUndo = 0, keySkip = 0;      // Virtual-key codes, 0 = none
    };

    struct View
    {
        Timer timer;
        Comparison comparison;      // Of the category (and level, for individual levels) being run or shown
        std::string categoryLabel;  // "Any%", "IL: Flight"
        std::vector<std::string> segmentNames;
        bool loading = false;       // Paused for a load right now
        bool newPb = false;         // The finished run is a personal best
        bool hooked = false;        // Auto splits work (the game's level completed event is watched)
        std::string map;            // Package name of the level the player is in
        std::string lastEvent;      // "Split: Flight", for the menu
        double now = 0.0;           // Clock the snapshot was taken at
    };

    double Now();                   // Seconds, QueryPerformanceCounter

    void SetOptions(const Options& o);  // Any thread
    void Tick();                        // Game thread, every tick (the module's OnTick)
    View Snapshot();                    // Any thread

    // Commands (any thread): run on the next tick.
    void StartOrSplit();
    void Reset();
    void Undo();
    void SkipSplit();
    void ForgetPb();                // Of the category shown

    // Chapter names in story order ("Prologue", "Flight", ... "The Shard") and the title of a map ("" if not a chapter).
    const std::vector<std::string>& Chapters();
    std::string ChapterTitle(const std::string& map);
    int ChapterIndex(const std::string& map);   // 0..9, -1
}
