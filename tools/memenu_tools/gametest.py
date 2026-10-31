"""`memenu game-test`: run the menu's in-game self-test (src/selftest.cpp) in the real game and report the result.

The unit tests and `verbose-build`'s test host can't see whether the menu works in Mirror's Edge: the test host has no
engine, so every game feature does nothing there and passes. This does it in the game:

1. copies the loader (dinput8.dll) and the menu into the game folder, like the installer
2. writes `memenu\\selftest` (the test's options) and, with --late, `memenu\\load-delay-ms` so the menu is loaded
   after the game went fullscreen (what the launcher does)
3. starts MirrorsEdge.exe; the menu runs the test, unloads and closes the game
4. reports every step from `memenu\\selftest-result.json`, the errors in the menu's log and, after a crash, where it
   happened (Windows' crash record resolved through the PDB)

No administrator rights needed as long as the game folder is writable and the game doesn't run as administrator
(Steam's default; compatibility mode "Run as administrator" breaks it, and the menu's own key handling too).
"""

from __future__ import annotations

import json
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from . import devtest

DEFAULT_GAME_DIRS = [
    Path(r"C:\Program Files (x86)\Steam\steamapps\common\mirrors edge\Binaries"),
    Path(r"C:\Program Files\Steam\steamapps\common\mirrors edge\Binaries"),
]


def steam_library_dirs() -> list[Path]:
    """Every Steam library's `steamapps\\common\\mirrors edge\\Binaries` (from libraryfolders.vdf)."""
    out = []
    for steam in (Path(r"C:\Program Files (x86)\Steam"), Path(r"C:\Program Files\Steam")):
        vdf = steam / "steamapps" / "libraryfolders.vdf"
        if not vdf.exists():
            continue
        for line in vdf.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.strip().split('"')
            if len(parts) >= 4 and parts[1] == "path":
                out.append(Path(parts[3].replace("\\\\", "\\")) / "steamapps" / "common" / "mirrors edge" / "Binaries")
    return out


def find_game_dir() -> Path | None:
    for d in DEFAULT_GAME_DIRS + steam_library_dirs():
        if (d / devtest.GAME_EXE).exists():
            return d
    return None


class ElevationRequired(Exception):
    """Windows wants administrator rights to start the game (compatibility settings on the exe)."""


@dataclass
class Result:
    finished: bool = False
    passed: bool = False
    current: str = ""
    steps: list[dict] = field(default_factory=list)
    crashes: list[devtest.Crash] = field(default_factory=list)
    log_errors: list[str] = field(default_factory=list)
    exit_reason: str = ""
    seconds: float = 0.0


def install(game_dir: Path, dll: Path, loader: Path) -> None:
    menu_dir = game_dir / "memenu"
    menu_dir.mkdir(exist_ok=True)
    shutil.copy2(loader, game_dir / "dinput8.dll")
    shutil.copy2(dll, menu_dir / "medge_menu.dll")
    pdb = dll.with_suffix(".pdb")
    if pdb.exists():
        shutil.copy2(pdb, menu_dir / "medge_menu.pdb")


def write_options(game_dir: Path, options: dict[str, str], late_ms: int) -> None:
    menu_dir = game_dir / "memenu"
    text = "".join(f"{k}={v}\n" for k, v in options.items() if v not in (None, ""))
    (menu_dir / "selftest").write_text(text, encoding="utf-8")
    delay = menu_dir / "load-delay-ms"
    if late_ms > 0:
        delay.write_text(str(late_ms), encoding="utf-8")
    elif delay.exists():
        delay.unlink()


def cleanup(game_dir: Path) -> None:
    for name in ("selftest", "load-delay-ms"):
        p = game_dir / "memenu" / name
        if p.exists():
            p.unlink()


def run(game_dir: Path, timeout: float, on_progress=None) -> Result:
    """Starts the game, waits for the self-test to finish and the game to close (or `timeout` seconds)."""
    menu_dir = game_dir / "memenu"
    result_file = menu_dir / "selftest-result.json"
    if result_file.exists():
        result_file.unlink()
    start = datetime.now()
    t0 = time.monotonic()
    try:
        proc = subprocess.Popen([str(game_dir / devtest.GAME_EXE)], cwd=game_dir)
    except OSError as e:
        if getattr(e, "winerror", None) == 740:   # ERROR_ELEVATION_REQUIRED
            raise ElevationRequired() from e
        raise
    res = Result()
    last_current = None
    while True:
        time.sleep(1.0)
        data = _read(result_file)
        if data:
            res.finished, res.passed = data.get("finished", False), data.get("passed", False)
            res.current, res.steps = data.get("current", ""), data.get("steps", [])
            if on_progress and res.current != last_current:
                last_current = res.current
                on_progress(res)
        if proc.poll() is not None:
            finished = (_read(result_file) or {}).get("finished", False)
            res.exit_reason = ("game closed by the self-test" if finished and proc.returncode == 0
                               else f"game exited (code {proc.returncode:#x})")
            break
        if res.finished:
            try:
                proc.wait(timeout=30.0)   # The self-test closes the game
                res.exit_reason = "game closed by the self-test"
            except subprocess.TimeoutExpired:
                proc.kill()
                res.exit_reason = "the game did not close 30 s after the self-test finished: killed"
            break
        if time.monotonic() - t0 > timeout:
            res.exit_reason = f"timed out after {timeout:.0f} s: game killed"
            proc.kill()
            break
    res.seconds = time.monotonic() - t0
    time.sleep(2.0)   # Windows writes its crash record a moment later
    res.crashes = devtest.crashes_since(start)
    log = devtest.log_since(menu_dir / "medge_menu.dll", start) or []
    res.log_errors = [line for line in log if "] [error]" in line or "] [critical]" in line]
    data = _read(result_file)
    if data:
        res.finished, res.passed = data.get("finished", False), data.get("passed", False)
        res.current, res.steps = data.get("current", ""), data.get("steps", [])
    return res


def _read(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def compat_layers() -> str:
    """The compatibility layers set on MirrorsEdge.exe for this user ("" if none), e.g. "~ RUNASADMIN WINXPSP3"."""
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                             r"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers")
        i = 0
        while True:
            name, value, _ = winreg.EnumValue(key, i)
            i += 1
            if name.lower().endswith("\\" + devtest.GAME_EXE.lower()):
                return str(value).strip()
    except OSError:
        return ""
