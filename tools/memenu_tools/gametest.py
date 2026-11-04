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

import ctypes
import json
import shutil
import subprocess
import time
from ctypes import wintypes
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
    environment: str = ""     # Not the menu's fault (the game or Steam didn't get going): worth another try

    @property
    def ok(self) -> bool:
        return self.finished and self.passed and not self.crashes


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


def _module_loaded(pid: int, name: str) -> bool | None:
    """Whether a DLL called `name` is loaded in process `pid` (None: couldn't tell)."""
    psapi, kernel32 = ctypes.WinDLL("psapi", use_last_error=True), ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                           ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)     # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    if not handle:
        return None
    try:
        modules = (wintypes.HMODULE * 1024)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 0x03):
            return None   # LIST_MODULES_ALL (the game is 32-bit)
        buf = ctypes.create_unicode_buffer(260)
        for i in range(min(needed.value // ctypes.sizeof(wintypes.HMODULE), 1024)):
            if psapi.GetModuleBaseNameW(handle, modules[i], buf, 260) and buf.value.lower() == name.lower():
                return True
        return False
    finally:
        kernel32.CloseHandle(handle)


def _close_windows(pid: int) -> None:
    """Asks the game to quit: WM_CLOSE to its top-level windows."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [proc_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
    user32.GetWindow.restype = wintypes.HWND
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and not user32.GetWindow(hwnd, 4):   # GW_OWNER
            user32.PostMessageW(hwnd, 0x0010, 0, 0)    # WM_CLOSE
        return True

    user32.EnumWindows(proc_type(each), 0)


def run(game_dir: Path, timeout: float, on_progress=None, close: bool = True) -> Result:
    """Starts the game, waits for the self-test to finish and the game to close (or `timeout` seconds).

    `close`: quit the game once the menu has unloaded. This is done from here and not by the menu: a game told to quit
    while the menu is still unloading itself can deadlock on the way out (the process exit and the menu's own FreeLibrary
    both want the loader lock), which left the game hanging every few runs."""
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
        time.sleep(0.25)
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
            if close:
                # Wait for the menu to be gone from the process, then quit the game
                for _ in range(30):
                    if _module_loaded(proc.pid, "medge_menu.dll") is not True or proc.poll() is not None:
                        break
                    time.sleep(0.5)
                if proc.poll() is None:
                    _close_windows(proc.pid)
            try:
                proc.wait(timeout=30.0)   # The game quits
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
    steam_error = _steam_error_window()
    clean = res.finished and proc.returncode == 0
    if not clean:
        time.sleep(2.0)   # Windows writes its crash record a moment later
    res.crashes = devtest.crashes_since(start)
    log = devtest.log_since(menu_dir / "medge_menu.dll", start) or []
    res.log_errors = [line for line in log if "] [error]" in line or "] [critical]" in line]
    data = _read(result_file)
    if data:
        res.finished, res.passed = data.get("finished", False), data.get("passed", False)
        res.current, res.steps = data.get("current", ""), data.get("steps", [])
    res.environment = _environment_problem(res, steam_error)
    return res


def _environment_problem(res: Result, steam_error: str) -> str:
    """Why the run failed without the menu being at fault, or "" when it wasn't that (or didn't fail)."""
    if res.ok or any("medge_menu" in c.module.lower() for c in res.crashes):
        return ""
    if steam_error:
        return f"Steam showed an error ({steam_error})"
    if not res.steps and not res.finished:
        return "the game ended before the menu started its test (it never got that far)"
    first = res.steps[0] if res.steps else {}
    if first.get("name") == "frames" and not first.get("ok", True):
        return "the game never drew a frame"
    return ""


def _steam_error_window() -> str:
    """The title of a visible Steam error window, if there is one ("Steam Error", the P:0000065432 load error)."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [proc_type, wintypes.LPARAM]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    found: list[str] = []

    def each(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            title = buf.value
            if "steam" in title.lower() and "error" in title.lower():
                found.append(title)
        return True

    user32.EnumWindows(proc_type(each), 0)
    return found[0] if found else ""


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
