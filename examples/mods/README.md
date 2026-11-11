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
```

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
