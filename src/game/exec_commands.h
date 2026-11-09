#pragma once
// The game's own console commands: UnrealScript `exec` functions (CheatManager.Slomo, PlayerController.Pause, ...),
// read from the engine's loaded classes, for the menu console's completion, highlighting and `help`.
//
// The flags of a UFunction aren't where the SDK says (its UField/UStruct layout is off, see uobject.h), so the offset
// is found once against functions whose flags are known (Slomo and God are exec, Object.Sin native, Actor.Tick an
// event). If that check fails the list is empty and the console falls back to its built-in engine commands.
#include <string>
#include <vector>

namespace game {
    struct ExecCommand {
        std::string name;       // "Slomo"
        std::string owner;      // Declaring class: "CheatManager"
        std::string params;     // "float T" (empty: none, or the parameter layout could not be checked)
    };

    // Scanned on the first call (game thread: walks the object list once, ~10k functions), then cached.
    const std::vector<ExecCommand>& ExecCommands();
}
