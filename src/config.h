#pragma once
namespace config {
    bool Load();        // MedgeMenu.json next to the DLL -> theme name + modules (enabled, key, settings)
    bool Save();        // modules -> MedgeMenu.json
    // Render thread, every frame: writes the config about once a second when something in it changed (a key bound, a
    // setting moved, a module switched), so quitting the game without unloading the menu loses nothing.
    void AutoSave();
    void SetAutoSave(bool on);  // Off: Save/AutoSave write nothing (the self-test, which must not change the player's config)
}
