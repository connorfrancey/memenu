#pragma once
namespace config {
    bool Load();   // MedgeMenu.json next to the DLL -> theme name + modules (enabled, key, settings)
    bool Save();   // modules -> MedgeMenu.json
}
