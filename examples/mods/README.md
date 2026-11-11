# Example Mirage mods

A mod is a folder in the menu's `mods` folder (`<game>\Binaries\memenu\mods\<id>`), named after its id, with a
`mod.toml` and its code. Both examples add one module, "Hello", to the menu's **Mods** column. While it is on it draws
a greeting, counts the game's ticks, and counts how many times it was switched on in its own `config.toml`.

| Folder       | Kind   | How to install                                                                     |
|--------------|--------|------------------------------------------------------------------------------------|
| `hello-lua/` | Lua    | Copy the folder into `mods`. No build.                                             |
| `hello/`     | Native | Build it (below), then copy `<build>/mod/hello` into `mods`.                       |

## mod.toml

```toml
id = "hello"              # a-z, 0-9, '-', '_'; the folder's name
name = "Hello"
version = "1.0.0"         # semver
mirage = "^0.1"           # runtime versions it works with
entry = "hello.dll"       # native code (or lua = "main.lua", the default when main.lua exists)

[dependencies]            # other mods, with version ranges ("^1.2", ">=1.0 <2", "*")

[settings.greeting]       # a setting, shown in the module's settings panel; the menu's config keeps its value
type = "string"           # bool, int, float, string, choice or key
name = "Greeting"
description = "What it draws"
default = "Hello"

[settings.height]
type = "float"
default = 0.2
min = 0.0                 # int and float need a range
max = 0.95

[settings.color]
type = "choice"
options = ["Blue", "Orange", "White"]
default = "Blue"
```

Declared settings go into the mod's first module, or into the module named by `module = "..."`. Code reads them by
key: `mirage::Get<std::string>("greeting")` in C++, `mirage.setting("greeting"):get()` in Lua. A module can also make
its own settings in code (`mirage::Slider` members, `module:slider(...)`), which are listed after the declared ones.

The full list of keys is in `src/runtime/manifest.h`. Unknown keys are reported in the menu's log.

## Native mods

Include `api/mirage/mirage.hpp`, which is a header-only C++ wrapper over the C ABI in `api/mirage/api.h`. Write
modules the way the menu's own are written, then add `MIRAGE_MOD()` once:

```cpp
class Hello final : public mirage::Module {
public:
    Hello() : Module("Hello (native)", "Mods", "Draws a greeting") {}
    void OnRender() override { mirage::DrawText(20, 200, mirage::Rgba(255, 255, 255), greeting.Get()); }
private:
    mirage::Text greeting{this, "Greeting", "Hello"};
};
MIRAGE_MODULE(Hello)
MIRAGE_MOD()
```

The game is 32-bit, so build the DLL for x86. Only C types cross into the menu, so the compiler doesn't have to match
the menu's: MSVC and clang-cl both work.

```
cmake -S examples/mods/hello -B build/hello -G Ninja -DCMAKE_BUILD_TYPE=Release     # from an x86 developer prompt
cmake --build build/hello
```

## Lua mods

`main.lua` runs in a sandbox: there is no `io` or `os`, and `require` only loads files from the mod's own folder. It
gets the same API as native mods:

```lua
local m = mirage.module { name = "Hello (Lua)", on_render = function()
    mirage.draw.text(20, 240, mirage.rgba(255, 200, 120), Greeting:get())
end }
Greeting = m:text("Greeting", "Hello from Lua")
```

`on_tick` runs on the game thread and `on_render` on the render thread. If a callback raises an error, the error is
logged and its module is switched off.

## Load order, phases and hot reload

- Mods load after the mods in their `[dependencies]` and `[optional]` lists, within their `phase`. A mod that can't
  load (a missing dependency, a cycle, a runtime it doesn't support) is left out, and the menu's log says why.
- `phase = "early"` loads at the menu's start, `"engine"` (the default) once the game layer is ready, and `"world"` when
  the first level has loaded.
- Save a Lua mod's file and the mod reloads by itself: its modules stay on, and their settings keep their values. A
  native mod does the same with `hot_reload = true`. Its DLL runs from a copy, so a new build can replace it while the
  game runs. Its `OnDisable` must undo everything the mod changed.
- If a mod crashes, it is stopped and named in the log, and the rest of the menu goes on. The crash is written to
  `mods\crashes.toml`, and the mod isn't loaded again until its entry there is deleted.

## The game: objects, events and hooks

Lua:

```lua
local pawn = mirage.pawn()                       -- the player (nil while loading); mirage.controller(), mirage.world()
pawn.Location.Z = pawn.Location.Z + 500          -- properties and struct members by name
pawn.Velocity = { X = 0, Y = 0, Z = 0 }          -- a struct from a table
mirage.call(pawn, "SetLocation", { NewLocation = pawn.Location })   -- a function, with its parameters by name

mirage.on("level_load", function(finished, map) if finished then mirage.log("now in", map) end end)
-- events: tick (seconds), level_load (finished, map), death (after), player_state (new, old), key (vk), state_restored

local hook = mirage.hook("Engine.Actor.CheckMaxEffectDistance", function(call)
    call.params.CullDistance = 0                 -- before the function: change its parameters...
    -- call:skip()                               -- ...or don't run it
end, { when = "before", priority = 10 })         -- "after" sees call.params.ReturnValue; "replace" sets it
hook:remove()
```

C++ (`mirage.hpp`): `mirage::Object::Pawn().Set("Location.Z", 500.f)`, `mirage::On(MIRAGE_EVENT_TICK, ...)`,
`mirage::Hook("Engine.Actor.SetLocation", MIRAGE_HOOK_BEFORE, 0, [](mirage::Call& c) { ... })`, and for native code
`mirage::FindPattern(...)` and `mirage::Detour(target, &Mine, priority, &original)`. Several mods can detour one
function, and they run by priority.

Hooks run on the game thread. A mod's hooks, events and detours go away when it unloads, reloads or crashes. A TAS
rewind puts the game's memory back but never a mod's: after `state_restored`, look game objects up again.

## Content mods: data, no code

A mod with `content = ["content"]` in its `mod.toml` changes the game through files in that folder, and gets a module
named after it that turns them on and off.

| Folder        | What it shows                                                                   |
|---------------|---------------------------------------------------------------------------------|
| `high-steps/` | `content/defaults.toml`: default property overrides (the player steps up higher) |
| `retexture/`  | `content/textures/M_Rooftops.Textures.T_Roof_02_D.png`: a replaced texture       |

A replacement texture is named by the texture's full path (outer packages and groups, dots between them). PNG, TGA, BMP
and JPG images are encoded to the original's format with a full mip chain; a DDS (DXT1/3/5, 32-bit ARGB or 8-bit
luminance) is used as stored. Sides must be powers of two. The game's files are never written: the packages holding the
texture are opened as rewritten copies kept in `memenu\cache\textures`. A change shows on the next load of a level not
loaded yet this session, and everywhere after a restart.

