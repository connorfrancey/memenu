# medge-menu

An internal mod menu, trainer and TAS / macro tool for Mirror's Edge (2008, PC), written in C++.

- [Installing](#installing)
- [Using the menu](#using-the-menu)
  - [The basics](#the-basics)
  - [What's in each tab](#whats-in-each-tab)
  - [Macros and TAS](#macros-and-tas)
  - [Files the menu keeps](#files-the-menu-keeps)
  - [Troubleshooting](#troubleshooting)
- [Developing](#developing)
  - [Setup and the `memenu` tool](#setup-and-the-memenu-tool)
  - [Repository layout](#repository-layout)
  - [How the menu works](#how-the-menu-works)
  - [Writing a module](#writing-a-module)
  - [Features and the staged build](#features-and-the-staged-build)
  - [Working with the game](#working-with-the-game)
  - [The TAS internals](#the-tas-internals)
  - [Testing](#testing)
  - [Logs, diagnostics and developer mode](#logs-diagnostics-and-developer-mode)
  - [Releasing](#releasing)

---

## Installing

Paste this into PowerShell and accept the administrator prompt (the game lives in Program Files):

```powershell
irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1 | iex
```

Then start Mirror's Edge the way you always do. The menu loads with the game. Run the same command again to update.

<details>
<summary>Other ways, and uninstalling</summary>

- **Without PowerShell:** download `memenu_installer.exe` from the [latest release](https://github.com/connorfrancey/memenu/releases/latest),
  run it and click **Install**. If it doesn't find your game, click **Browse** and pick `MirrorsEdge.exe`.
- **Game not found automatically:**
  `& ([scriptblock]::Create((irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1))) -GameDir "D:\Games\Mirror's Edge"`
- **Uninstall:** the same with `-Uninstall` (add `-RemoveSettings` to delete your settings too), or **Uninstall** in the installer window.
- **Without installing:** `medge_launcher.exe` injects the menu into a game that is already running.

</details>

What it does: it puts a small loader (`dinput8.dll`) next to `MirrorsEdge.exe` and the menu in a `memenu` folder beside
it. The game itself is not modified, so Steam updates and "Verify integrity of game files" leave it alone. To start the
game without the menu once, create an empty file called `disabled` in the `memenu` folder. **Uninstall** in the
installer puts everything back (and offers to keep your settings).

---

## Using the menu

### The basics

| Key        | What it does                                   |
|------------|------------------------------------------------|
| **INSERT** | open / close the menu                          |
| **END**    | unload the menu (the game keeps running)       |

The menu is a row of columns (tabs). Every row in a column is a **module**:

- **Left click** a module to switch it on or off. Some modules are *actions* (Save State, Restart Checkpoint, ...):
  clicking runs them once.
- **Right click** a module to open its settings. They are saved with the rest of your config.
- Every module has a **Toggle key** / **Hotkey** button at the bottom of its settings. Click it, then press the key
  you want (Escape cancels). The key works while the menu is closed.
- Hover over a module or a setting for a description.

While the menu is open the game doesn't get your mouse and keyboard, so you can click around without the camera
spinning. Settings are written to `MedgeMenu.json` when the menu unloads (END, or closing the game), or right away
with **Misc → Save Config**.

### What's in each tab

Default keys are in brackets. All of them can be changed.

**Player**
- **No Clip** [F5]: fly through everything. **Flight**: fly but still collide with the world. They share the same
  controls: WASD to move, Space / Left Ctrl up / down, Left Shift to boost, and settings for speed, responsiveness,
  "move along view" and invulnerability. Only one of them can be on at a time.
- **God Mode**: take no damage.
- **Teleport to Spawn**: back to the level's start.

**Trainer**: tools from the mmultiplayer trainer
- **Save State** [4] / **Load State** [5]: remember position, speed, rotation and current move, and go back to it.
- **Speedometer**: position, speed, angles and more in a small window. Its checkpoint key [3] marks a split.
- **Kickglitch**, **Beamer**, **Strang**: one-shot tools. Switch one on to arm it. It does its move at the right moment,
  then switches itself off. **Tool Overlay** lists the armed tools in the top-right corner.
- **Refill Reaction Time**.

**TAS**: **TAS / Macro**, see [Macros and TAS](#macros-and-tas).

**World**
- **Game Speed**: slow motion or faster.
- **Freeze Timer**: stops the time trial clock.

**Visual**: **Crosshair**, **FOV**, **Show FPS**.

**Debug**: **Collision View** (draws collision and trigger volumes), **Performance** (frame times, the module
scheduler, caches), **Log Viewer** (the menu's log, filterable).

**Levels**
- **Levels**: every chapter, listed last to first (the final chapter at the top, the Prologue at the bottom). Click a chapter to open it, then
  **Start of level** or a checkpoint to load it. *Console load* uses `open <map>` instead of the game's menu.
  *Save progress* lets those loads overwrite your saved progress (off by default).
- **Restart Checkpoint**, **Main Menu**, **Sublevels** (load / show / hide the level's streaming parts).

**Misc**: **Themes** (pick a color theme, or reload the theme files), **Save Config** / **Load Config**,
**Watermark**, **Game Status** (checks the game integration, useful in bug reports).

### Macros and TAS

> TAS / Macro is not in default builds yet (release builds included): build the menu with
> `uv run memenu build --enable tas`, or turn the feature on in `cmake/Features.cmake`.

The **TAS / Macro** module records everything you press and plays it back. It works like Geometry Dash bots such as
Mega Hack's replay bot: pick a mode, play, and save the macro under a name. Behind that is a real TAS engine: the game
runs on a fixed time step, so a playback does exactly what the recording did, frame for frame.

#### The panel

```
 [  Off  ] [ Record ] [  Play  ]        mode: the lit one is the current one
 [ macro name        ] [Save] [Load]    save / load under that name
 REC 1234  0:20.567                     status: frame, game time
 [ Pause ] [ Frame advance ]
 [ Rewind] [ Take over     ]
 [ 0     ] [ Go to frame   ]
 -- New recording --                    how the next recording starts
 -- Playback --                         speed, what happens at the end
 -- Macros --                           Save to file... / Load from file..., saved macros, Delete, New
 -- Keys --                             hotkeys while a macro runs
 -- Tools --                            overlay, replay editor, key translator
```

#### Recording a macro, step by step

1. Open **TAS / Macro** (right click it) and look at **New recording → Start**:
   - **Level / checkpoint** (recommended): pick a chapter and checkpoint. Recording loads it and starts on the first
     frame after the load. Every playback starts the same way, so the run is **reproducible and verified**.
   - **Console command**: same idea with any command, e.g. `open edge_p`.
   - **Now (macro)**: starts right where you are. Handy for a short trick you want to repeat, but the world isn't
     reset, so it can't be checked and practice mode doesn't work.
2. Pick a **Frame rate**. 60 is a good default. *Real time* stores each frame's real length instead (for speedruns
   recorded at normal speed).
3. Click **Record** and play. The overlay in the top-left corner shows `REC <frame>`.
4. Click **Off** (or press **F8**) to stop.
5. Type a name and click **Save**. It goes to `replays/<name>.merp` in the `memenu` folder.

To play it back, click **Play**. Your own input is ignored until it ends. When it has finished, the overlay says
**verified** if every frame matched the recording, or **desync @ frame N** at the first frame that didn't.

#### Practice mode (rewind on death)

With a level start, **On death** decides what happens when you die while recording:

| Setting               | What happens                                                                                   |
|-----------------------|------------------------------------------------------------------------------------------------|
| **Rewind (practice)** | the default. The last **Rewind by** seconds (2 s) are cut. The macro replays up to that point at full speed and **pauses**. Press **Pause/resume** (F5) and keep going. The macro only keeps the attempt that worked, like GD's practice mode. |
| **Restart attempt**   | the recording is thrown away and starts over from the start level.                             |
| **Keep recording**    | the death and respawn stay in the macro.                                                       |

You can also rewind any time with **Rewind** (F9), for example after a sloppy jump. Every rewind counts as a
*rerecord* (shown in the file info).

#### Hotkeys while a macro runs

| Key | Action |
|-----|--------|
| **F5** | Pause / resume |
| **F6** | Frame advance: pauses, then each press runs exactly one frame |
| **F7** | Take over: during playback, keep the macro up to this frame and record from here |
| **F8** | Stop |
| **F9** | Rewind (practice) |

These keys never reach the game while a macro runs, so they are never recorded. INSERT and END aren't either.
F5 is also No Clip's default key: if you use both, give one of them another key.

#### Playback options

- **Speed** (or **As fast as possible**): 0.5 for slow motion while studying a run, 8x to skip ahead. It doesn't
  change the result, only how fast it is shown.
- **At the end**: *Stop*, *Pause* (the last frame stays on screen) or *Keep recording*, to extend a macro.
- **Pause on desync**: stop at the first frame that differs, to see what went wrong.
- **Go to frame**: plays as fast as possible up to that frame, then pauses. Combine it with **Take over** to redo
  everything after a frame (the TAS workflow).
- **Deterministic streaming** (on): loads streamed level parts on the frame they are requested, so every PC gets them
  on the same frame. A recording needs the same setting to play back.

#### Saving and loading files

- **Save** / **Load** next to the name: `replays/<name>.merp` in the `memenu` folder.
- **Save to file... / Load from file...**: Windows' save / open dialog, so you can put the macro anywhere and share it.
  Save as `.merp` (compact, with the sync data that verification needs) or `.json` (readable and editable). Loading
  accepts both. In fullscreen the dialog may open behind the game: Alt+Tab to it.
- **Macros** list: every saved macro, newest first, with a search box. Click one to select it, double click to load
  it. **Delete** (click twice) removes the selected file. **New** empties the macro in memory.
- **Export JSON...** writes the macro as text.

The JSON format, for writing macros by hand or with a script:

```json
{
  "framerate": 60,
  "start": { "kind": "checkpoint", "map": "Prologue (edge_p)", "checkpoint": "" },
  "frames": 120,
  "inputs": [
    { "frame": 0,  "key": "W" },
    { "frame": 30, "key": "SpaceBar" },
    { "frame": 32, "key": "SpaceBar", "event": "up" },
    { "frame": 40, "key": "MouseX", "event": "axis", "value": 3.5 },
    { "frame": 90, "key": "W", "event": "up" }
  ]
}
```

Only `inputs` is required. `event` is `down` (default), `up`, `repeat`, `double` or `axis`. `start.kind` is `now`
(default), `command` (with `command`) or `checkpoint` (with `map` and `checkpoint`, labels as the Levels tab shows
them, `""` = level start). Keys use Unreal's names: `W`, `A`, `SpaceBar`, `LeftShift`, `LeftControl`,
`LeftMouseButton`, `MouseX`, `MouseY`, ... Inputs are keys, not actions. A macro made with other key bindings can be
fixed with **Tools → key translator** (e.g. `LeftMouseButton` → `RightMouseButton`).

#### Replay editor

**Tools → Replay editor** opens a window with every frame and its inputs. Select a frame to change its inputs or
length, add or delete frames, or jump there with **Go to**. Frames after an edit are no longer "verified" until the
next playback records their state again.

#### What breaks a macro

- Modules that change the game (No Clip, God Mode, Game Speed, ...): the panel warns when one is on. Turn them off for
  recording **and** playback.
- A different game version (the file stores it, and you'll get a warning).
- A "Now" macro started from a different place or state.

### Files the menu keeps

Everything is in the `memenu` folder next to `MirrorsEdge.exe`:

| Path                         | What                                                             |
|------------------------------|------------------------------------------------------------------|
| `MedgeMenu.json`             | your settings: theme, every module's on/off state, keys, options |
| `themes/*.json`              | color themes; edit or add your own, then **Misc → Themes** to reload |
| `replays/*.merp`             | saved macros / runs                                              |
| `logs/memenu.log`            | this session's log; older ones are compressed in `logs/.old/`    |
| `disabled`                   | create this (empty) file to start the game without the menu      |

### Troubleshooting

- **The menu doesn't open:** make sure the game isn't running as administrator or in a compatibility mode (right
  click `MirrorsEdge.exe` → Properties → Compatibility: untick "Run as administrator" and "Windows XP"). Look at
  `logs/memenu.log`.
- **A feature says "unavailable" or does nothing:** **Misc → Game Status** shows which parts of the game integration
  were found.
- **Jump doesn't work after flying:** fixed in current builds (the jump block of No Clip/Flight could stay set after
  a load or death). Update, or toggle No Clip on and off once.
- **Reporting a bug:** include `logs/memenu.log` and, if you build it yourself, the output of
  `uv run memenu --copy-info`.

---

## Developing

### Setup and the `memenu` tool

You need Visual Studio with "Desktop development with C++", CMake 4.2+, Ninja and [uv](https://docs.astral.sh/uv/).
The game is a 32-bit program, so everything is built for x86. The tool sets up the 32-bit MSVC environment itself,
so any terminal works.

```
uv sync                       # creates .venv with the project tool
uv run memenu info            # toolchain, build folders, detected game
uv run memenu build           # debug build (-c release for release); build.bat does the same
uv run memenu errors          # build everything, list errors grouped by file
uv run memenu test            # unit tests (GoogleTest)
uv run memenu install-game    # build and install into the game
uv run memenu launch          # inject into a running game without installing
uv run memenu package         # dist/memenu_installer-<version>.exe
uv run memenu features        # every feature and whether it is built
uv run memenu --copy-info     # diagnostic info (logs, builds, game, crashes) to the clipboard
uv run memenu verbose-build   # build stage by stage and test every stage without the game
uv run memenu game-test       # run the menu's self-test inside Mirror's Edge
```

`build.bat [debug|release] [clean] [options]` is a shortcut for `memenu build` that also works without uv (with
Python, click and rich installed). The DLL ends up in `build/medge_menu.dll`. The build folders are
`cmake-build-x86-<config>`.

A typical loop: edit, `uv run memenu build`, start the game, `uv run memenu launch` (or `install-game` once and just
restart the game), END to unload, repeat. Run `uv run memenu test` before committing, and `uv run memenu game-test`
when you changed something that touches the game.

### Repository layout

| Folder / file          | What                                                                         |
|------------------------|------------------------------------------------------------------------------|
| `src/dllmain.cpp`      | entry point: starts the menu on its own thread, unload                       |
| `src/hooks.cpp`        | D3D9 (EndScene / Reset) and WndProc hooks, input blocking                    |
| `src/gui.cpp`          | the click-GUI: columns, module rows, settings panels                         |
| `src/core/`            | module system, settings / controls, keybinds, scheduler, caches, notifications |
| `src/features/<tab>/`  | one file per module, grouped by tab                                          |
| `src/game/`            | game integration on top of the Mirror's Edge SDK: player, levels, flight, object lookups |
| `src/tas/`             | TAS engine (`engine`), sessions (`tas`), the replay format (`replay`), scripted macros (`macro.h`) |
| `src/render/`          | debug renderer and meshes (Collision View)                                   |
| `src/minimal/`         | the bare hook used by the first stages of the staged build                   |
| `src/selftest.cpp`     | the in-game self-test (`memenu game-test`)                                   |
| `src/config.cpp`, `theme*.cpp`, `log.cpp` | settings file, themes, logging                            |
| `loader/`              | `dinput8.dll` proxy that loads the menu with the game                        |
| `installer/`           | `memenu_installer.exe`, with both DLLs embedded                               |
| `launcher/`            | `medge_launcher.exe`: injects into a running game (no install)                |
| `tests/`               | unit tests (GoogleTest), run without the game                                |
| `tools/memenu_tools/`  | the `memenu` Python tool; `tools/testhost/` is the fake D3D9 game for `verbose-build` |
| `cmake/`               | dependencies (`Libraries.cmake`), feature switches (`Features.cmake`), build info, patches for the SDK |

### How the menu works

- **Threads.** The menu has two threads that matter. The **render thread** is the game's D3D9 EndScene hook: ImGui
  runs there, and so do `OnRender` and every control. The **game thread** is the engine's tick: anything that
  touches game objects runs there (`OnTick`, `game::RunOnGameThread`). Don't read or write UObjects from the render
  thread. Post the work to the game thread instead.
- **Modules** (`src/core/module.h`) register themselves with `MEM_MODULE(T)` from their own .cpp. There is no
  central list. A module has a tab, an order, a description, optional default key and group (modules of a group
  switch each other off, like No Clip / Flight).
- **Controls** (`src/core/setting.h`) are members of the module: `ctl::Checkbox`, `ctl::Slider`, `ctl::SliderInt`,
  `ctl::Combo`, `ctl::DynamicCombo`, `ctl::ColorPick`, `ctl::KeyHold`, `ctl::Button`, `ctl::Label`,
  `ctl::Separator`, `ctl::Custom`. They register with their module, are drawn in declaration order, are saved to
  `MedgeMenu.json` by name, and can have key bindings. `VisibleIf` hides a control based on others.
- **Scheduler** (`src/core/scheduler.h`): `OnTick` runs according to the module's `schedule` (interval, priority,
  budget). Use `Priority::Critical` for anything that must happen on an exact frame. `NeedsUpdate()` and
  `MarkDirty()` skip work when nothing changed.
- **Config** (`src/config.cpp`): theme name plus every module's state, key and controls, in `MedgeMenu.json` next
  to the DLL (`paths::Dir()`).

### Writing a module

Create `src/features/<tab>/my_thing.cpp`:

```cpp
#include "core/module.h"
#include "game/game.h"

#include <ME_SDK.hpp>

namespace {

class MyThing final : public Module {
public:
    MyThing() : Module("My Thing", "Player", "One line shown on hover", 50)
    {
        key = VK_F10;                                   // optional default toggle key
    }

    void OnTick() override                              // game thread, every tick while on
    {
        if (auto* pawn = game::Pawn(); pawn && pawn->Health > 0 && pawn->Health < minHealth)
            pawn->Health = minHealth;
    }

    void OnDisable() override                           // undo what you changed; may run on any thread
    {
        game::RunOnGameThread([] { /* restore any game values you changed */ });
    }

private:
    ctl::SliderInt minHealth{this, "Minimum health", 50, 1, 100};
};
MEM_MODULE(MyThing)

} // namespace
```

Then register it in `cmake/Features.cmake` under its category:

```cmake
memenu_feature(my_thing          player GAME STAGE 9 FILES features/player/my_thing.cpp)
```

`GAME` means it needs the game integration. `STAGE` is where it comes in in the staged build (see the stage list at
the top of the file). Add `OFF` to leave it out of default builds. Re-run `uv run memenu build`, and the module shows
up in its tab.

Rules that keep the menu stable:
- Undo everything in `OnDisable`. It runs on unload too (END), and the game keeps running without the menu.
- Values the menu owns (input blocking, collision, god mode) must not leak into Save State; see
  `IsOwnedByMenu` in `src/game/player_state.cpp`.
- When you take something over from the game, remember the game's value and restore *that*, and only if you
  changed it. Track ownership separately from "active" so a load or a death in between can't make you save your own
  value as the game's (see `BlockButtons` in `src/game/flight.cpp`).
- Action modules derive from `ActionModule` and implement `OnTrigger`.
- Free-form UI goes in a `ctl::Custom` (see the TAS module), overlays in `OnRender` with
  `ImGui::GetForegroundDrawList()`.

### Features and the staged build

`cmake/Features.cmake` decides which modules go into the DLL: a master switch, one switch per category, one per
feature. From the tool:

```
uv run memenu build --enable tas                 # add TAS
uv run memenu build --only levels                # only the Levels tab
uv run memenu build --disable debug              # everything but the Debug tab
uv run memenu build --default-features           # back to the defaults
uv run memenu build --no-optimizations           # no caches / scheduler intervals (to rule them out)
```

The choice is remembered by the build folder. The **staged build** (`uv run memenu minimal --list`, `--stage N`,
`--next`) builds the menu up from a bare D3D9 hook (stage 0) to everything (the last stage), adding the riskier parts
last. That is what `verbose-build` walks through to find the first stage that breaks.

### Working with the game

- The SDK is [mmultiplayer](https://github.com/connorfrancey/mmultiplayer)'s Mirror's Edge SDK, fetched by
  `cmake/Libraries.cmake` and patched from `cmake/patches/`. Its `UField`/`UStruct` offsets are wrong: use
  `game/uobject.h` (`game::SuperOf`, `NextOf`, `ChildrenOf`, `game::IsA`), never `->IsA()`, `->Next` or `->SuperField`.
- `game::Pawn()`, `game::Controller()`, `game::World()` give the player and world (they can be null during loads and
  cutscenes). Check every time.
- `AActor::SetLocation()` reports success but doesn't move the player: write `pawn->Location` directly.
- Look objects up with `game::ObjectRef` (cached) instead of scanning the object list every tick. Object scans and
  SDK `FindObject` were the real per-tick costs in profiling.
- Hooks use MinHook. Patterns are found with `game::FindPattern(bytes, mask)`.
- `game::OnLevelLoad` tells you when a level starts and finishes loading. `game::IsLoading()` says whether one is
  loading now.

### The TAS internals

| File               | What                                                                                        |
|--------------------|---------------------------------------------------------------------------------------------|
| `tas/engine.*`     | hooks: the engine's frame timing, `InputKey`/`InputAxis`, `QueryPerformanceCounter` & co., `srand`; the virtual clock and input injection |
| `tas/tas.*`        | sessions: record / play / pause / advance / take over / seek / rewind, death handling, sync checks, files |
| `tas/replay.*`     | the `.merp` format: chunked binary with a checksum, editing helpers, JSON export / import |
| `tas/macro.h`      | `tas::Macro`: write a macro in code (`Wait`, `Press`, `Release`, `Hold`, `Axis`) |
| `features/tas/tas_module.cpp` | the panel, the file dialog, the replay editor                                    |

How a session works: while one runs, the game thread sees a virtual 10 MHz clock that advances by exactly one frame's
time step per engine tick, `rand()` is seeded at the start, and streamed sublevels load on the frame they are
requested. Real input is held back to the start of the next frame (recording), or dropped and replaced by the
replay's (playing), so both see identical input. Each frame stores the player's position, velocity and move, and a
playback compares them bit for bit.

Rewind (practice mode) is built from existing pieces: truncate the replay, start a playback with a seek to the cut,
and when the seek arrives switch to recording (`recordAfterSeek` in `tas.cpp`) and pause. Commands from other threads
are posted and run on the game thread at the start of the next frame. Everything from the UI goes through
`tas::WithReplay` (locked) or the session functions in `tas.h`.

The `.merp` layout is documented at the top of `src/tas/replay.cpp`: magic `MERP`, version, tagged chunks
(`HEAD`, `META`, `NAME`, `TIME`, `EVNT`, `SYNC`, `EXTN`), then `END ` with an FNV-1a checksum. Unknown chunks are
skipped, so new data can be added without breaking old readers. Bump `kVersion` only for changes old readers can't
skip.

### Testing

- **Unit tests** (`tests/`, `uv run memenu test`): math, meshes, the replay format (save/load, damage detection,
  editing, JSON round trip and import), macros. They don't need the game. Add a test for anything that can run
  without it.
- **`verbose-build`**: builds every stage, checks it (32-bit, no Visual C++ runtime DLLs, warnings) and loads it twice
  into `memenu_testhost.exe`, a small D3D9 program that stands in for the game: draws with the menu closed and open,
  resets the device, unloads. It writes `build/verbose-build/<time>/report.md` and stops at the first failing stage.
- **`game-test`**: the only test that proves game features work. It installs the build, starts the game, and the menu
  tests itself (`src/selftest.cpp`): drawing, the engine tick, loading Prologue checkpoint 2 (checkpoints 0/1 are the
  intro cutscene, which pins the player), input blocking, every module switched on and off with its controls pressed,
  checks of what it does to the game (God Mode keeps health full, Load State puts a moved player back, a TAS macro
  plays back, ...), everything at once, and a clean unload. The game closes by itself and the tool prints every step,
  the log's errors and, after a crash, the crashing source line. Options: `--late 20` (load 20 s late, like the
  launcher), `--only` / `--skip` to pick modules. It needs a writable game folder and a game that doesn't run
  elevated. Self-test checks must not pass by gravity or time alone: wait in ticks, and check something only the
  feature can cause.

### Logs, diagnostics and developer mode

The menu logs to `logs/memenu.log` next to its DLL. Each start compresses the previous log with zstd into
`logs/.old/memenu_<date-time>_b<build number>_<magic>.log.zst` (the newest 100 are kept). Every log starts with a
header: when the session started, the build number and the build's *magic*, a hash of its details (version, commit,
configuration, compiler, options, features). The format is described in `src/log.cpp`.

Set `MEMENU_DEV=1` for developer mode: full tracebacks with local variables, timings, and `memenu dev ...`
(`env`, `symbolize`, `logs`, `crashes`, `pe`, `host`, `unload`; see `tools/memenu_tools/devmode.py`).

### Releasing

1. Bump `set(MEMENU_VERSION ...)` in `CMakeLists.txt` and commit.
2. `git tag v<version>` and `git push origin v<version>`.

The [release workflow](.github/workflows/release.yml) builds, tests and publishes `memenu_installer.exe` with its
`SHA256SUMS.txt`. That is what the install command downloads, so players get it the next time they run it. It refuses
to publish when the tag and `MEMENU_VERSION` differ.
