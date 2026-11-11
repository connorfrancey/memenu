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

On Linux the game runs in Steam's Proton: it is started with `steam steam://rungameid/17410` (Steam handles Proton and
the game's DRM), found in /proc, and the menu's unload is seen in /proc/<pid>/maps. The game's Steam launch options
need WINEDLLOVERRIDES="dinput8=n,b" (install.sh says so too), or Proton uses its own dinput8.dll and the menu never
loads: checked before starting (linux_launch_options). See doc/linux-development.md.
"""

from __future__ import annotations

import ctypes
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from . import devtest, hangmon

LINUX = sys.platform.startswith("linux")
if not LINUX:
    from ctypes import wintypes
APP_ID = "17410"        # Mirror's Edge on Steam

DEFAULT_GAME_DIRS = [
    Path(r"C:\Program Files (x86)\Steam\steamapps\common\mirrors edge\Binaries"),
    Path(r"C:\Program Files\Steam\steamapps\common\mirrors edge\Binaries"),
]


def steam_roots() -> list[Path]:
    """Where Steam is installed: Windows, or Linux (native, the Debian/Ubuntu package's ~/.steam, Flatpak)."""
    if not LINUX:
        return [Path(r"C:\Program Files (x86)\Steam"), Path(r"C:\Program Files\Steam")]
    home = Path.home()
    roots = [home / ".steam" / "steam", home / ".local" / "share" / "Steam", home / ".steam" / "debian-installation",
             home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam"]
    seen, out = set(), []
    for r in roots:
        try:
            real = r.resolve()
        except OSError:
            continue
        if real.is_dir() and real not in seen:
            seen.add(real)
            out.append(real)
    return out


def steam_library_dirs() -> list[Path]:
    """Every Steam library's `steamapps\\common\\mirrors edge\\Binaries` (from libraryfolders.vdf)."""
    out = []
    for steam in steam_roots():
        vdf = steam / "steamapps" / "libraryfolders.vdf"
        if not vdf.exists():
            continue
        for line in vdf.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.strip().split('"')
            if len(parts) >= 4 and parts[1] == "path":
                out.append(Path(parts[3].replace("\\\\", "\\")) / "steamapps" / "common" / "mirrors edge" / "Binaries")
    return out


def find_game_dir() -> Path | None:
    for d in ([] if LINUX else DEFAULT_GAME_DIRS) + [r / "steamapps" / "common" / "mirrors edge" / "Binaries"
                                                     for r in steam_roots()] + steam_library_dirs():
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
    hang: "hangmon.Hang | None" = None   # The game stopped making progress and was ended (tools/memenu_tools/hangmon.py)
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


def linux_launch_options() -> str | None:
    """The game's Steam launch options (localconfig.vdf of every Steam user here, joined), None if Steam has none set.
    Linux only: there they must hand Proton WINEDLLOVERRIDES="dinput8=n,b"."""
    found = []
    for root in steam_roots():
        for vdf in (root / "userdata").glob("*/config/localconfig.vdf"):
            text = vdf.read_text(encoding="utf-8", errors="replace")
            # "17410" { ... "LaunchOptions" "..." ... }: the first LaunchOptions after the app's key, within its block
            for m in re.finditer(r'"' + APP_ID + r'"\s*\{', text):
                block = text[m.end():m.end() + 4000]
                end = block.find("\n\t\t\t\t}")
                if end > 0:
                    block = block[:end]
                lo = re.search(r'"LaunchOptions"\s*"((?:[^"\\]|\\.)*)"', block)
                if lo:
                    found.append(lo.group(1).replace('\\"', '"'))
    return " | ".join(found) if found else None


def _linux_module_loaded(pid: int, name: str) -> bool | None:
    """Whether Wine has `name` mapped in the process (it maps a PE DLL from its file), None if unknown."""
    try:
        maps = Path(f"/proc/{pid}/maps").read_text(errors="replace").lower()
    except OSError:
        return None
    return name.lower() in maps


def _module_loaded(pid: int, name: str) -> bool | None:
    if LINUX:
        return _linux_module_loaded(pid, name)
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
    if LINUX:       # Wine ends the process on SIGTERM; the menu has unloaded by then (its results are on disk)
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
        return
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


class _LinuxGame:
    """The game started through Steam on Linux, with the parts of subprocess.Popen that run() uses."""

    def __init__(self, start_timeout: float = 90.0):
        before = set(devtest.game_pids())
        subprocess.Popen(["steam", f"steam://rungameid/{APP_ID}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        self.pid = 0
        self.returncode: int | None = None
        deadline = time.monotonic() + start_timeout
        while time.monotonic() < deadline and not self.pid:
            new = [p for p in devtest.game_pids() if p not in before]
            if new:
                self.pid = new[0]
            else:
                time.sleep(0.5)
        if not self.pid:
            raise OSError(f"Steam did not start Mirror's Edge within {start_timeout:.0f} s (is Steam running and "
                          f"logged in, with the game installed and Proton chosen?)")

    def poll(self) -> int | None:
        if self.returncode is None and not Path(f"/proc/{self.pid}").exists():
            self.returncode = 0         # Not ours to wait for: the exit code is Steam's
        return self.returncode

    def wait(self, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        while self.poll() is None:
            if time.monotonic() > deadline:
                raise subprocess.TimeoutExpired("MirrorsEdge.exe", timeout)
            time.sleep(0.25)
        return self.returncode or 0

    def kill(self) -> None:
        for pid in devtest.game_pids() or [self.pid]:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


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
        proc = _LinuxGame() if LINUX else subprocess.Popen([str(game_dir / devtest.GAME_EXE)], cwd=game_dir)
    except OSError as e:
        if getattr(e, "winerror", None) == 740:   # ERROR_ELEVATION_REQUIRED
            raise ElevationRequired() from e
        raise
    res = Result()
    last_current = None
    watch = None if LINUX else hangmon.HangWatch(proc.pid, menu_dir / "logs" / "memenu.log", menu_dir / "logs")
    while True:
        time.sleep(0.25)
        data = _read(result_file)
        if data:
            res.finished, res.passed = data.get("finished", False), data.get("passed", False)
            res.current, res.steps = data.get("current", ""), data.get("steps", [])
            if on_progress and res.current != last_current:
                last_current = res.current
                on_progress(res)
        hang = watch.check(res.current) if watch and not res.finished else None
        if watch and watch.finished is not None and not res.finished:
            # The menu logged the end but its result file says otherwise (a write lost): the log decides
            res.finished, res.passed = True, watch.finished
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
        if hang and not res.finished:
            # Stuck: what is known about it written down, then the game ended (not the whole timeout, not the failsafe)
            res.hang = watch.record(hang)
            res.exit_reason = f"{res.hang.describe()}: game ended"
            proc.kill()
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
    if LINUX:
        return ""
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
    if LINUX:
        return ""
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
