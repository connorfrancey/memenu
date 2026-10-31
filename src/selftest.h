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
namespace selftest {
    bool Requested();   // Reads (and deletes) the `selftest` file. Call once, after logging::Init
    void Run();         // Menu thread, after the hooks are installed. Returns when done (or when unload is requested)
    void Finish(bool unloadedCleanly);   // After hooks::Uninstall: final step, writes the result, closes the game
}
