# Mirage: from menu to mod loader

Mirage turns the Mirror's Edge menu into a mod loader in the spirit of Fabric (a loader plus a hook system, with
everything else in mods) and Geode (a native runtime with a stable API, an in-game mod manager and tooling). The menu's
own features become the first mods built on it. UnrealLib (`C:\dev\cpp\lib\UnrealLib`, the C++ UELib port) is the
foundation for everything that has to understand the game's packages and script. Mods are written in **Lua** (the
default: no compiler, hot reload, sandboxed) or as native DLLs (for what Lua is too slow or too limited for); both use
the same API.

## Where we start

| Have | Gap for a mod loader |
|------|----------------------|
| `dinput8.dll` proxy loader, installer, launcher | Loads one DLL; no discovery, ordering or dependencies |
| `medge_menu.dll`: 43 modules registered at build time (`MEM_MODULE`), settings, config, hotkeys, themes | Nothing can be added without rebuilding the DLL; no ABI |
| Game layer: pattern scans, Tick / ProcessEvent / death / level load hooks | Private to the DLL; one hook owner per function |
| mmultiplayer SDK (generated headers) | Wrong `UField`/`UStruct` layouts, 1-byte bools where the engine has 4-byte UBOOLs (`IsInState`, `SetMove`, ...) |
| UnrealLib M0–M9: packages, linker, properties, reflection, bytecode decoder, decompiler, assets, package writer, T3D | C++20 (menu is C++17); no bytecode encoder; cooked packages hold no property memory offsets |
| Self-test in the real game (`memenu game-test`), 142 unit tests | Nothing that loads third-party code |

## Target architecture

```
MirrorsEdge.exe
 └─ dinput8.dll ............ loader: memory tracker, early phase, loads mirage.dll
     └─ mirage.dll ......... runtime: engine layer, reflection, hooks, events, VFS, mod manager UI
         ├─ Lua 5.4 ....... one sandboxed state per Lua mod
         ├─ mods\memenu\ ... the current features, as first-party mods (trainer, TAS, Info Lines, ...)
         └─ mods\<id>\ ..... mod.json + main.lua and/or <id>.dll + content\ (packages, textures, script patches)
```

- **Stable C ABI** (`mirage/api.h`: a versioned function table, plain structs, no STL across the boundary) with a
  header-only C++ wrapper (`mirage/mirage.hpp`), so mods built with another compiler version keep working.
- **Lua is the main way to write a mod.** Lua 5.4, compiled as C++ (errors unwind C++ frames instead of `longjmp`
  over them), one sandboxed state per mod (no `io`/`os.execute`/`package.loadlib`; files only inside the mod's folder),
  bound to the same API as the C ABI. Game objects are reached through the live reflection of step 2, so a Lua mod can
  read and write any property and call any function by name without generated bindings. Why not LuaJIT: Lua 5.4 builds
  from source with the project's CMake and static CRT, and its error handling interoperates with C++; LuaJIT's speed is
  not needed for mod logic, and the hot paths (hooks on functions no mod hooks) never enter Lua.
- **Mods need not contain code:** content and script patches are data that the runtime applies through UnrealLib.
- **The rules that cost crashes so far stay rules:** no work under the loader lock, the rendering thread, the
  62 fps cap, the TAS memory ownership (see the project memory notes).

## The plan

Each step ends in a milestone with a check that proves it; a milestone is committed when its check passes.

### 1. Groundwork: C++20, UnrealLib in the process, runtime split

- Move the menu to C++20 and link UnrealLib (x86, static CRT `/MT` like the rest; its CMake already supports both).
- Split the source tree: `runtime/` (what becomes `mirage.dll`), `api/` (public headers, empty for now), `mods/memenu/`
  (the features, still linked in for this step).
- Load Core.u, Engine.u and TdGame.u with UnrealLib inside the game, lazily and off the game thread; measure time and
  memory (32-bit process: the TAS snapshots already use a lot of it).

- Build Lua 5.4 from source (as C++, static CRT, x86) into the DLL and run a script in the game: the toolchain and
  the error path (a Lua error caught as a C++ exception, no leak) proven before any API is built on it.

**Milestone:** `mirage.dll` builds with UnrealLib and Lua linked; full `game-test` passes; the log shows the three
script packages parsed in the game with their class counts, the time taken and the memory used, within a set budget,
and a Lua script run in the game (including a caught error).

**Status (2026-11-09): done, except the source tree split** (`src/runtime/` holds the new code; the move of the rest
into `runtime/` / `api/` / `mods/memenu/` waits for step 9, when the features become mods). C++20, UnrealLib and Lua 5.4
(compiled as C++) are linked. In the game (self-test "Mirage runtime"): Core, Engine and TdGame read in 0.07 s (release),
2,039 classes with 11,169 properties, 44 MB while loaded and nothing left after (counted by the menu's own operator
new, `src/runtime/memory.cpp`: the process's numbers move with the game's allocations); 13 Lua checks (sandbox, memory
limit, instruction budget, C++ destructors run when a Lua error unwinds).

### 2. A live engine model built on UnrealLib

- `mirage::Object / Class / Property / Function` handles over the live `GObjects`, typed with UnrealLib's
  `ReflectionSchema` (property types, enums, structs, function signatures from the game's own packages).
- Property access by name with type checks; function calls through `ProcessEvent` with parameter buffers sized and laid
  out from the engine's own `UFunction` (UBOOL = 4 bytes, return value last): the fix for the `IsInState` class of bugs.
- Compute memory layouts from UnrealLib (super's size, property sizes and alignment) and verify them against the live
  `UProperty::Offset` values (`player_state.cpp` already finds that field).

**Milestone:** every property of every class in Core, Engine and TdGame: computed offset == live offset (0 mismatches,
or each one explained and handled); `IsInState`, `SetMove` and the other bool-parameter calls go through the new path;
the self-test passes.

**Status (2026-11-09): done.** Self-test "Mirage layouts": all 13,481 classes, structs, functions and states UnrealLib
reads, and all 33,987 of their properties (offset, size, array size, bool bit), match the engine. The 70 the engine
has on top are intrinsic native types never exported to a package (Core.Class, Core.Field, the property classes,
Engine.World, Engine.Level, Engine.StaticMesh and its two structs, the net drivers, ...). The link rules, found by the
comparison (`src/runtime/computed_layout.cpp`): byte aligned to 1, everything else to 4, structs to at least 4 and
Matrix to 16; consecutive bools share a dword except in functions; struct and class sizes are not padded, but a struct
property's element size is; a map property is 60 bytes. Self-test "Mirage calls": `src/runtime/call.h` lays parameters
out from the engine's UFunction and runs functions with a native index too: UE3's ProcessEvent refuses those (that is
why the SDK's IsInState and SetLocation "returned" garbage), so the index (UFunction+0x94, checked against IsInState =
281 and SetLocation = 267) is cleared for the call. The menu's SDK calls that were wrong now go through it
(`src/runtime/engine_calls.h`): SetMove (its flags came from stack garbage and its return value was written past the
SDK's struct), SetRotation and MoveSmooth (never ran: Teleport to Spawn now really turns the view, checked by its self-test;
Flight's collision mode calls MoveSmooth, which now runs, not yet covered by a test), ToggleReactionTime, SetSoundMode, StartGame, RestartFromLastCheckpoint.

### 3. The SDK generator

- Offline tool (UnrealLib + the layouts from step 2) that writes typed C++ headers: classes with correct offsets,
  UBOOL bitfields, enums, structs, function wrappers with correct parameter structs.
- Also writes the decompiled UnrealScript of each class next to its header (the reference mod authors read), and Lua
  type stubs (LuaLS `---@class` annotations) for every game class, so Lua mods get completion and type checks in an
  editor while the runtime itself stays reflection-driven.
- Replace the mmultiplayer SDK in the menu's own code.

**Milestone:** the generated SDK compiles with zero warnings; the menu builds on it with no mmultiplayer headers left;
`game-test` passes; a report lists every layout the old SDK had wrong.

### 4. Mod format and ABI v1

- `mod.json`: id, semver version, name, authors, runtime version range, game build, dependencies (required / optional /
  incompatible, with version ranges), entry DLL, load phase, settings schema, content folders.
- `mirage/api.h` v1: logging, the module and settings system (today's `ctl::` controls) and per-mod config, exposed
  through the ABI; the C++ wrapper gives mods the same `MEM_MODULE` / `ctl::Slider` style as today.
- The Lua API (`mirage` table) over the same calls: `mirage.module{...}`, settings, log, config, plus game object
  access through reflection (`player.Location.Z = 500`, `pawn:SetMove("Walking")`). One binding layer, so the C ABI
  and Lua can't drift apart.
- ABI tests: struct sizes and offsets frozen, the table only grows.

**Milestone:** a "hello" mod built in a separate CMake project (and with another MSVC toolset) and the same mod
written in Lua are both loaded, register a module with settings that save and load, and show in the menu; the ABI
tests pass.

### 5. Loader and mod lifecycle

- Discovery in `mods\`, manifest validation, dependency resolution (topological order, semver ranges, cycles, missing
  or incompatible mods are disabled with a reason shown in the menu).
- Load phases: `early` (from the dinput8 loader, before the engine: memory tracking, file redirects), `engine` (SDK
  globals and hooks ready), `world` (first level loaded); unload in reverse order.
- Fault isolation: SEH around every call into a mod; a crash is attributed to the mod whose code faulted (address to
  module) and that mod is disabled at the next start, with the report.
- Lua mods: one state per mod, loaded from `main.lua`, with a memory limit and an instruction budget per callback
  (a runaway script is stopped and named, not left to freeze the game). Hot reload is the normal case for Lua (the file
  changed: the mod is reloaded); native mods only when they declare they support it.
- TAS: mod memory is classified (the TAS snapshot memory map) so rewinds don't corrupt mods; mods can opt in to
  having their state rewound.

**Milestone:** unit tests over synthetic manifests (chains, cycles, version conflicts, optional deps); in the game,
a set of test mods loads in the right order, a deliberately crashing mod is caught, named and disabled, and the rest
keep running; a hot reload round trip works ten times in a row.

### 6. Hooks and events

- Event bus: tick, render, level load, death, input, player state change, script events (the existing hooks, now
  shared).
- UFunction hooks: before / after / replace, for script and native functions (through `ProcessEvent` and the native
  function pointer), keyed by the function, with a fast path so unhooked calls stay cheap (the per-tick cost lessons
  in the performance notes).
- Managed native detours (MinHook behind the runtime): several mods on one function, in priority order, conflicts
  reported, everything removed on unload.
- Lua gets events and UFunction hooks (with the parameters as a table, writable for "before" hooks); native detours
  stay native-only.

**Milestone:** two mods (one Lua, one native) hook the same UFunction, both run in priority order and unload
cleanly; the `ProcessEvent` hook costs under a set budget per call for unhooked functions and for a Lua hook (measured
in the performance module).

### 7. Content mods through UnrealLib

- A virtual file system for `TdGame\CookedPC`: the engine's package opens are redirected to overlay packages that
  the runtime builds with UnrealLib's writer, cached by the hash of their inputs.
- What a mod can ship: replacement textures, sounds and meshes (UnrealLib's asset code, reversed), `defaultproperties`
  and property overrides as JSON, map edits as T3D (UnrealLib's T3D import).
- The game folder is never written: removing the mod removes its changes.

**Milestone:** an example mod that replaces a texture and one that changes default properties work in the game with
no game file touched; turning them off restores the original on the next level load; UnrealLib's corpus sweeps still
pass.

### 8. Script mods: bytecode assembly and patching

- Add a bytecode encoder to UnrealLib (the inverse of its decoder) and prove it on the corpus: every function of every
  package decodes and re-encodes byte-identical.
- Runtime script patching: replace a function's bytecode in the loaded `UFunction` (script, ScriptSize, label table),
  and mixin-style injection (call a mod's function at the start, before each return, or around a call site).
- Also offline: patched script baked into overlay packages (step 7) for patches that must exist before the level loads.

**Milestone:** 100% byte-identical encode round trip over the Mirror's Edge corpus; an example mod changes a script
function's behaviour (for example the jump logic in TdPawn) with no native detour, and turning it off restores it.

### 9. The menu becomes mods, and a mod manager

- The current features move into first-party mods (`memenu-trainer`, `memenu-tas`, `memenu-visual`, ...) that use only
  the public API; anything they need that the API lacks is added to the API, not reached around. Simple ones (God
  Mode, Game Speed, Teleport to Spawn) are rewritten in Lua as the examples mod authors copy; the TAS and the
  collision view stay native.
- The menu becomes the mod manager: installed mods, enable/disable, per-mod settings and hotkeys, dependency and
  conflict view, each mod's log and crash reports.

**Milestone:** the first-party mods compile against `api/` headers only (enforced by the build: no runtime include
paths); every one of today's self-test checks passes with the features loaded as mods.

### 10. Tooling, docs and distribution

- `mirage` CLI (the `memenu` tool grown): new mod from a template (Lua or native), build, package, install into the
  game, run the in-game test harness for a mod (Lua mods can carry their own in-game tests).
- Docs: getting started, the API reference, the generated SDK and decompiled script, examples for each kind of mod.
- Installer and launcher handle mods; a mod index (a JSON registry in a Git repository) with hashes, versions and
  compatibility; update checks in the mod manager.

**Milestone:** someone who has never seen the code builds, tests and publishes a mod with only the docs and the CLI;
a release candidate installs, finds a mod in the index, installs and updates it.

## Order and dependencies

```
1 ─► 2 ─► 3 ─────────────► 9 ─► 10
     │         4 ─► 5 ─► 6 ─┘
     └──────────► 7 ─► 8
```

Steps 4–6 (the loader) and 7–8 (content and script through UnrealLib) can run in parallel once step 2 exists. Step 9
waits for 3 and 6 because the features need the generated SDK and shared hooks.

## Risks

| Risk | Plan |
|------|------|
| UnrealLib in a 32-bit process next to TAS snapshots runs out of address space | Load only script packages, lazily; measure in step 1 and keep a budget |
| Computed layouts disagree with the engine for native classes | Step 2 checks every offset live; native-only fields come from the live engine, not the packages |
| ABI drift breaks old mods | Frozen struct tests, append-only function table, runtime refuses mods built for a newer ABI |
| Mods hooking the same thing fight | One owner (the runtime) for every hook; priorities; conflicts shown in the mod manager |
| Third-party code crashes the game | SEH isolation, attribution, auto-disable; the existing crash logger names the mod |
| Byte-identical bytecode encoding is harder than decoding (jump offsets, labels) | Prove it on the full corpus before any runtime patching relies on it |
| A Lua mod freezes or bloats the game | Per-mod instruction budget and memory limit; the mod is stopped and named |
| Lua in hot hooks costs frame time | Lua runs only for functions a Lua mod hooks; the cost per call is measured (step 6) |

## Decisions

- Scripting: Lua 5.4, compiled as C++ (2026-11-09).

## Open questions

- The file format for a packaged mod (a folder only, or also a zip such as `.mirage`).
- Whether the mod index should be curated or open.
