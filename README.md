# medge-menu
An internal mod menu for Mirror's Edge, written in C++

## Installing (players)

Paste this into PowerShell and accept the administrator prompt (the game lives in Program Files):

```powershell
irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1 | iex
```

Then start Mirror's Edge the way you always do. The menu loads with the game: **INSERT** opens it, **END** unloads it.
Run the same command again to update.

<details>
<summary>Other ways, and uninstalling</summary>

- **Without PowerShell:** download `memenu_installer.exe` from the [latest release](https://github.com/connorfrancey/memenu/releases/latest),
  run it and click **Install**. If it doesn't find your game, click **Browse** and pick `MirrorsEdge.exe`.
- **Game not found automatically:**
  `& ([scriptblock]::Create((irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1))) -GameDir "D:\Games\Mirror's Edge"`
- **Uninstall:** the same with `-Uninstall` (add `-RemoveSettings` to delete your settings too), or **Uninstall** in the installer window.

</details>

What it does: it puts a small loader (`dinput8.dll`) next to `MirrorsEdge.exe` and the menu in a `memenu` folder beside
it. The game itself is not modified, so Steam updates and "Verify integrity of game files" leave it alone. The menu's
settings, themes and logs live in that `memenu` folder. To start the game without the menu once, create an empty file
called `disabled` in it. **Uninstall** in the installer puts everything back (and offers to keep your settings).

## Building (developers)

Needs Visual Studio with "Desktop development with C++", CMake 4.2+, Ninja and [uv](https://docs.astral.sh/uv/).

```
uv sync                       # creates .venv with the project tool
uv run memenu info            # toolchain, build folders, detected game
uv run memenu build           # debug build (-c release for release)
uv run memenu errors          # build everything, list errors grouped by file
uv run memenu test            # unit tests (Catch2)
uv run memenu install-game    # build and install into the game
uv run memenu launch          # inject into a running game without installing
uv run memenu package         # dist/memenu_installer-<version>.exe
```

The tool sets up the 32-bit MSVC environment itself, so any terminal works.

### Releasing

1. Bump `set(MEMENU_VERSION ...)` in `CMakeLists.txt` and commit.
2. `git tag v<version>` and `git push origin v<version>`.

The [release workflow](.github/workflows/release.yml) builds, tests and publishes `memenu_installer.exe` with its
`SHA256SUMS.txt`. That is what the install command downloads, so players get it the next time they run it. It refuses
to publish when the tag and `MEMENU_VERSION` differ.

| Folder       | What                                                              |
|--------------|-------------------------------------------------------------------|
| `src/`       | the menu (`medge_menu.dll`)                                       |
| `loader/`    | `dinput8.dll` proxy that loads the menu with the game             |
| `installer/` | `memenu_installer.exe`, with both DLLs embedded                    |
| `launcher/`  | `medge_launcher.exe`: injects into a running game (no install)     |
| `tests/`     | unit tests                                                        |
| `tools/`     | the `memenu` Python tool                                          |
