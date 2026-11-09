#pragma once
// In-game self-test: proves the menu works inside the real game, which the unit tests and the test host can't.
//
// It runs when a file called `selftest` is next to medge_menu.dll when the menu starts (`memenu game-test` writes it,
// starts the game and reads the result). The file is deleted as soon as it is read, so a test that crashes the game
// doesn't run again on the next normal start. Optional `key=value` lines in it:
//
//   map=edge_p          level to load before the module tests (matched against the level list, case-insensitive;
//                       "none" stays where the game starts)
//   checkpoint=2        where in it: 0 = level start, n = its n-th checkpoint (default 2, "After_Intro": the
//                       Prologue's start is its intro cutscene, which holds the player in place)
//   ticks=10            how many engine ticks (and frames) each module stays on
//   only=God Mode,FOV   test only these modules (default: every module)
//   skip=Main Menu      don't test these
//   quit=1              close the game when done (default 1)
//
// What it does, step by step (each one logged as "Self-test: ..." and written to `selftest-result.json` as it goes,
// so a crash still shows which step it was in):
//   frames      EndScene is hooked and the menu draws (frames keep coming, menu open and closed)
//   game        the engine tick hook runs
//   level       the chosen level loads and the player pawn appears
//   module:X    every module, on the render thread like a click: switched on with its settings panel open, every
//               key-bindable control pressed, menu closed and opened again, switched off. Fails on: the game or the
//               frames stopping, the scheduler marking it failed, an error in the log, a crash.
//   all-on      every module on at once
//   perf        what the menu costs per tick and per frame (from the scheduler and caches)
//   unload      hooks removed and the menu unloaded cleanly (written by dllmain after the unload)
// Module states and settings are put back as they were before the test.
//
// Developer mode can also run it from the menu (the Test module, features/debug/self_test.cpp): Start() runs the
// chosen tests on a thread of their own, in the game as it is, without unloading or closing anything afterwards.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace selftest {
    bool Requested();   // Reads (and deletes) the `selftest` file. Call once, after logging::Init
    void Run();         // Menu thread, after the hooks are installed. Returns when done (or when unload is requested)
    void Finish(bool unloadedCleanly);   // After hooks::Uninstall: final step, writes the result, closes the game

    // ---- Runs from the menu (developer mode) ----------------------------------------------------------------------------
    struct RunOptions {
        std::set<std::string> only;     // Feature test and module names; empty = everything (like `memenu game-test`)
        bool loadLevel = false;         // false: test where the player is now (a level must be loaded)
        std::string map = "edge_p";     // With loadLevel: which level, and where in it (see `checkpoint=` above)
        int checkpoint = 2;
        unsigned ticks = 10;            // Engine ticks (and frames) each module stays on
        bool fast = true;               // The 62 fps cap off while testing (put back afterwards)
    };
    struct TestInfo {
        std::string name;
        bool optIn;                     // Only runs when picked by name (measurements, not checks)
    };
    std::vector<TestInfo> FeatureTests();   // The feature tests this build has, in run order

    bool Start(RunOptions options);     // Render thread (a click). False while a run is going
    void Stop();                        // Ends the run at the next wait; settings are put back
    bool Running();
    void Shutdown();                    // Menu thread, at unload, before the hooks go: stops a run and waits for it

    struct StepView {
        enum class State { Running, Passed, Failed } state = State::Running;
        std::string name;
        double seconds = 0.0;
        std::vector<std::string> problems, notes;
    };
    struct Progress {
        bool running = false;
        bool stopped = false;           // The last run was stopped (Stop, or unload) before it finished
        int run = 0;                    // Runs started this session (0: none yet)
        int planned = 0;                // Steps the run expects (an estimate: for a progress bar)
        double seconds = 0.0;           // Of the current run, or the last one
        std::uint64_t firstLogId = 0;   // Log lines after this one belong to the run (logging::CopySince)
        std::vector<StepView> steps;
    };
    Progress Snapshot();                // Any thread
}
