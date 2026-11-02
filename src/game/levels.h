#pragma once
#include <string>
#include <vector>

// Level / Checkpoint / Sublevel loading on top of the mmultiplayer SDK.
// All lists are snapshots (updated on the game thread, guarded by a mutex) so the menu thread can read them freely.
namespace levels
{
    enum class Method { Game, Console };    // StartGame() of the game's own data store, or the console command "open <map>"

    struct MapView
    {
        std::string label;                      // "Prologue (edge_p)"
        std::vector<std::string> checkpoints;   // Checkpoint labels; The implicit "start of level" is not included
        std::string file;                       // Package name, "edge_p"
        std::vector<std::string> checkpointNames;   // The game's names of the checkpoints ("Scraper_Start")
    };

    struct SublevelView
    {
        std::string name;
        bool loaded = false;        // Streaming flag: Should be loaded
        bool visible = false;       // Streaming flag: Should be visible
    };

    void Init();        // Call after game::Init() succeeded: starts the background list refresh
    void Refresh();     // Rebuild the map and sublevel lists now (on the game thread)

    // Maps come from the game's own level-select data (UIDataProvider_TdMaps), with a built-in fallback list.
    std::vector<MapView> Maps();
    // Package name of the level the player is in ("Edge_p"), empty in none. Updated about twice a second.
    std::string CurrentMap();
    // Checkpoint: 0 = start of the level, n = the n-th entry of MapView::checkpoints.
    void Load(int map, int checkpoint, Method method, bool saveProgress);
    void RestartFromLastCheckpoint();
    void QuitToMainMenu();

    // Streaming sublevels of the level you are in.
    std::vector<SublevelView> Sublevels();
    void ToggleSublevelLoaded(const std::string& name);     // Load + show, or unload
    void ToggleSublevelVisible(const std::string& name);    // Show (loading it if needed) or hide

    std::string LastMessage();      // Result of the last action, for the on-screen status line
}