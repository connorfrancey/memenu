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
         └─ mods\<id>\ ..... mod.toml + main.lua and/or <id>.dll + content\ (packages, textures, script patches)
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

**Status (2026-11-10): done.** `memenu sdk` builds and runs
`mirage_sdkgen` (`tools/sdkgen/`, configured by `sdkgen.toml`, read with toml11) and compiles what it writes. Inputs:
the layouts computed from the packages (the same `runtime/computed_layout.cpp` the self-test proves) plus
`engine-layouts.json`, what only the engine knows (601 intrinsic and native-only types, saved by the self-test "Mirage
layouts"). Output, in 1.5 s: `sdk/` with 2,354 classes, 515 structs and all 11,310 functions (static array parameters
included), with every offset and size a `static_assert`. It compiles with no warnings at /W4. Bits are checked at run
time (`detail::CheckLayouts`). `build/sdk-reference/` holds the decompiled script of the 2,110 classes (2,107 with no
placeholders), the LuaLS stubs and `old-sdk-report.md`. The report lays out the old SDK's C++ the way MSVC does under
its `pack(4)` and compares it with the engine: 197 classes, 16 structs and 6,002 parameter structs wrong (IsInState's
return value at 0x9 instead of 0xC, SetMove's parameters 4 bytes instead of 16, the intrinsic classes too small).
Known gap: two HTTPDownload properties lie inside its native base's opaque block and can't be declared. The menu builds on
`sdk/` (committed; mmultiplayer is only downloaded for the report), with one source change (`StaticMesh` is now typed:
intrinsic classes are named by their import). The full `game-test` passes, and "Mirage layouts" also runs the SDK's
bitfield checks in the game. The "still draws after switching off" check had been failing for Kickglitch Helper
before this step too: Game Status and Collision View text changed under it, so they are switched off while a HUD is
measured.

### 4. Mod format and ABI v1

- `mod.toml` (TOML, read with toml11): id, semver version, name, authors, runtime version range, game build,
  dependencies (required / optional / incompatible, with version ranges), entry DLL, load phase, settings schema,
  content folders.
- `mirage/api.h` v1: logging, the module and settings system (today's `ctl::` controls) and per-mod config, exposed
  through the ABI; the C++ wrapper gives mods the same `MEM_MODULE` / `ctl::Slider` style as today.
- The Lua API (`mirage` table) over the same calls: `mirage.module{...}`, settings, log, config, plus game object
  access through reflection (`player.Location.Z = 500`, `pawn:SetMove("Walking")`). One binding layer, so the C ABI
  and Lua can't drift apart.
- ABI tests: struct sizes and offsets frozen, the table only grows.

**Milestone:** a "hello" mod built in a separate CMake project (and with another MSVC toolset) and the same mod
written in Lua are both loaded, register a module with settings that save and load, and show in the menu; the ABI
tests pass.

**Status (2026-11-11): done, with two items moved on.** `api/mirage/api.h` is ABI v1: a `MirageApi` function table
that starts with its size and only grows, a size-prefixed module description with five callbacks, settings
(checkbox, slider, int slider, text, key), per-mod `config.toml`, and drawing primitives (a native mod can't share the
menu's ImGui). `mirage.hpp` wraps it in the menu's own `Module` / `ctl::` style. `src/runtime/manifest` reads `mod.toml`
with toml11 and warns about unknown keys; `semver` handles versions and npm/Cargo ranges. `src/runtime/mods` runs each
mod module as a normal menu `Module`. Every call into native code is under SEH: a crash is logged and the module
stopped. The Lua binding (`mod_lua`) is written only against `MirageApi`. Its callbacks hold the mod's lock and get the
instruction budget, and `require` stays inside the mod's folder. Mods load in id order after their runtime range and
required and incompatible mods are checked; topological order is step 5.

The check: `tests/test_abi.cpp` freezes every offset; semver and manifest have unit tests. The in-game self-test
"Mirage mods" covers `examples/mods/hello`, built by its own CMake project with **clang-cl** instead of a second MSVC
toolset (only one is installed, and another compiler is the stronger proof), and `examples/mods/hello-lua`. Both load,
show in the Mods column, run `on_enable` / `on_tick` / `on_render` / `on_disable`, save, load and clamp their
settings, and keep their own config. `memenu game-test` builds and deploys them.

Settings can be declared in `mod.toml`, as Geode does in `mod.json` (2026-11-11): one `[settings.<key>]` table each,
with type (bool, int, float, string, choice, key), name, description, default and range, options or format. The runtime
makes them when their module registers, first in its panel, and the menu's config saves their values. Code finds them
by key: `setting_find` and `setting_type` are appended to ABI v1, Lua has `mirage.setting("key")`, C++ has
`mirage::Get<T>("key")`. Settings made in code still work alongside them.

Moved on: game object access from Lua through reflection (`player.Location.Z = 500`) goes with the hooks and events
of step 6, where Lua first gets game objects.

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

**Status (2026-11-11): done, except two parts.** `src/runtime/resolve` decides what loads and in what order:
dependencies first, then phase, then id. A mod is left out, with the reason, for a runtime out of range, being off or
having crashed, a missing, out-of-range, left-out or later-phase requirement, an incompatible mod, or a cycle of
required mods, and that spreads to the mods that need it. Optional dependencies only order (with warnings when they
can't), and optional cycles are broken. 13 unit tests cover chains, cycles, version conflicts, optional deps, phases and
the off list.

Phases: early at the menu's start, engine once the game layer is up, world on the first tick after a level loaded.
Mods of the later phases publish their modules as a batch: the menu's module list only grows once it runs (reserved,
count published atomically), the render thread adds them, gives them their saved config and the scheduler takes
them. The config now keeps the entries of modules not loaded this time.

Crash isolation: every call into native code is guarded. A crash stops the whole mod and logs its name, callback,
exception and `dll+offset`. It also writes `crashes.toml` next to the mods, which leaves the mod out at the next start
until its entry is deleted. The menu's crash handler names a mod whose code crashed on a thread it started.

Hot reload: Lua mods by default, native ones with `hot_reload = true` (their DLL runs from a copy in `.mirage`). A
watcher on the game thread checks the files twice a second and waits until they stop changing. A reload switches the
mod's modules off (its old `on_disable` undoes its changes), waits for calls still running, unloads, and loads again.
The modules it registers again are the same menu modules, so settings, keys and on/off state carry over.

Self-test "Mirage lifecycle" (`tests/mods`, deployed by `game-test`): order across the early and engine phases, the
cycle and missing-dependency mods left out, a world-phase mod, `crasher` (native, clang-cl) caught at
`crasher.dll+0x14E0` while the others go on and left out by the next resolution, Lua hot reload 10 times (new code
each time, setting and on state kept, 1 KB left after the first), the watcher, and native hot reload 10 times. One full
run of the five with this code crashed the game in `ntdll` during the "Speedrun timer" level restart, with no mod
involved; the four after it passed. Not explained yet.

Left: TAS memory classification for mods (the snapshot memory map; mods opting in to being rewound) waits for step 6,
where mods get game objects. Attributing crashes on a mod's own thread is in place but not covered by a test (it would
take the game down).

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

**Status (2026-11-11): done.** Function hooks (`src/runtime/fhooks`): every script function runs through
`UObject::ProcessInternal`, whether it is called from script, from native code or through ProcessEvent, so that one
function is detoured. A lock-free table keyed by the frame's function sends hooked calls to their hooks, and an
unhooked call costs a hash and a load (measured: 0.7-0.8 ns). Hooks run before, after or instead of the function, in
priority order, and see its parameters by name (`runtime/reflect`: properties, struct members and parameters through
the engine's own UProperty objects, by dotted path). Native functions get a per-function thunk in `UFunction::Func`:
before and after only, without parameters. One with a native index is called straight from bytecode and only reaches
its hooks through ProcessEvent, so its code is detoured instead. The offsets are found in the game: `Func` at 0xAC
(where every script function holds ProcessInternal's address), and the frame's `Node` at 0x4 and `Locals` at 0x10.

Mods get these by appending to ABI v1: events (tick, level load, death, player state, key, and a TAS rewind putting the
game's memory back), function hooks, calls, game objects with values by path, and managed native detours
(`runtime/detours`). Several mods can chain on one function in priority order; every `original` is relinked as
detours come and go, two mods on one target is logged, and a function the menu hooks itself is refused. 3 unit tests
cover the chains. A mod's hooks, events and detours run like its module callbacks: only while it runs, counted for
reloads, guarded against crashes, and removed when it unloads, reloads or crashes. In Lua: `mirage.on`, `mirage.hook`
(`call.params.CullDistance = 1`, `call:skip()`), `mirage.pawn() / controller() / world() / find()` with object and
struct proxies (`pawn.Location.Z = 500`), and `mirage.call`. In C++: `mirage::On`, `mirage::Hook`,
`mirage::Object`, `mirage::Call`, `mirage::Detour`, `mirage::FindPattern`.

TAS: snapshots never hold a mod's memory. Its DLLs, the copies hot reload runs, its heaps and its allocations are under
`memenu\`, which the memory map leaves out (checked by the self-test). Its changes to the game's own memory are rewound
with the game. The `state_restored` event tells mods to look their objects up again. Rewinding a mod's own state was
the "opt in" idea; it isn't done, because a mod can rebuild its state from that event.

The check, self-test "Mirage hooks from mods" (`tests/mods/hook-native`, `hook-lua`): both hook
`CheckMaxEffectDistance` while their modules are on, and run in priority order (each turns `CullDistance` into
x * 10 + its number: 1, then 12). They keep that through three hot reloads of both, leave no hook behind when off, read
the pawn's `Location.Z` by name, and get tick and level load events. Costs, also shown in the Performance module: a
call through ProcessEvent takes 0.18 us, a native hook adds 0.28 us (budget 5) and a Lua hook 1.12 us (budget 50).
Self-test "Mirage function hooks" covers the core: priority, parameters read and changed, after hooks, replace, a
call from script, removal.

Seen while testing: 2 of about 12 full runs this session crashed the game in `ntdll`'s heap code. One was during the
Speedrun timer level restart, the other during the TAS workflow, a free in the game's CRT heap (MSVCR80) with the AMD
driver on the stack, before any hook or detour existed. The runs around them passed. Not explained yet; a TAS rewind of
the game's CRT heap while the driver holds memory in it is the first suspect. To look into separately, against a build
from before Mirage.

### 7. Content mods through UnrealLib

- A virtual file system for `TdGame\CookedPC`: the engine's package opens are redirected to overlay packages that
  the runtime builds with UnrealLib's writer, cached by the hash of their inputs.
- What a mod can ship: replacement textures, sounds and meshes (UnrealLib's asset code, reversed), `defaultproperties`
  and property overrides as JSON, map edits as T3D (UnrealLib's T3D import).
- The game folder is never written: removing the mod removes its changes.

**Milestone:** an example mod that replaces a texture and one that changes default properties work in the game with
no game file touched; turning them off restores the original on the next level load; UnrealLib's corpus sweeps still
pass.

**Status (2026-11-11): done for defaults and textures.** Default property overrides work, as data applied at runtime
rather than baked into overlay packages. `content/defaults.toml` holds the overrides (`runtime/content`). They are
applied through reflection on the game thread, re-applied after level loads and undone exactly, and a module named after
the mod turns them on and off (`examples/mods/high-steps`, self-test "Mirage content").

Textures: `content/textures/<full texture path>.png` (or `.tga/.bmp/.jpg`, encoded to the texture's own format with a
mip chain; `.dds` as stored). A survey of the Prologue and menu maps settled the design: maps hold *copies* of the
textures they use, which stream their large mips from uncompressed storage packages by raw offset, and every texture
whose mips are all inline is NeverStream or resident. So a replaced texture is written as NeverStream with every mip
inline into each package that holds a copy (`runtime/texture_files`), and storage packages are never touched: the
append-only overlay planned above isn't needed. In the game (`runtime/texture_overlays`, `runtime/file_redirect`):

- An index of which packages hold which texture, from the package headers only (2,406 packages in 2.7 s, then cached
  by size and time: 0.2 s).
- A copy is made the first time the game opens a package that holds a replaced texture, in the opening thread
  (126-297 ms per Prologue map, written uncompressed: LZO took seconds), then cached by a hash of the original and the
  images. Building them all up front was tried and dropped: one rooftop texture is in 98 packages (1.35 GB).
- kernelbase `CreateFileW` / `GetFileAttributesExW` hooks open the copy instead. A package keeps the file it first got
  for the whole session: switching it under the engine, which keeps its files open by name, hung the loader. Turning
  textures on or off applies to packages not loaded yet, and to all of them after a restart.
- Uncompressed copies must clear `PKG_StoreCompressed`, or the engine crashes reading the summary.

`examples/mods/retexture` and the self-test "Mirage textures" show it (the live texture object has the image's size).
Left for later in this step: sounds, meshes and T3D map edits.

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
- Mod manifest: `mod.toml` (TOML, read with toml11), not JSON (2026-11-10).
- Mod settings are declared in `mod.toml` (`[settings.<key>]`), like Geode's `mod.json` (2026-11-11).

## Open questions

- The file format for a packaged mod (a folder only, or also a zip such as `.mirage`).
- Whether the mod index should be curated or open.
