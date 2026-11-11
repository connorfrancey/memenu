"""Hang manager for `memenu game-test`: notices when the game stops making progress, finds out why, records it and ends
the game, instead of waiting out the whole timeout (or for someone to press the failsafe).

What counts as hung (checked a few times a second by gametest.run):
  * a dialog window of the game's process (an assert box, a message box): the test can't go on until someone clicks it.
    Its title and text are the report;
  * the menu's own watchdog lines ("Hang: no frame for 8 s", "Stall: ... did not come") with no self-test progress
    for `grace` seconds after them;
  * the menu's log silent for `quiet` seconds while the game is still running (the menu itself stuck).

Before the game is ended: a minidump of it (threads and stacks, small) into the menu's logs folder, and the last thread
stacks the menu logged. Windows only for the dialog and the dump; elsewhere the hang is still detected and ended.
"""

from __future__ import annotations

import ctypes
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

WINDOWS = sys.platform == "win32"


@dataclass
class Hang:
    reason: str                                         # One line: what was seen
    step: str = ""                                      # The self-test step it happened in
    dialog: str = ""                                    # "title: text" of a dialog, if one was up
    stacks: list[str] = field(default_factory=list)     # The menu's last "Hang:/Stall: thread ..." lines
    dump: Path | None = None                            # The minidump written, if any

    def describe(self) -> str:
        where = f" in step {self.step}" if self.step else ""
        what = f"hung{where}: {self.reason}"
        if self.dialog:
            what += f" (dialog: {self.dialog})"
        if self.dump:
            what += f"; minidump {self.dump}"
        return what


class HangWatch:
    def __init__(self, pid: int, log: Path, dumps: Path, grace: float = 20.0, quiet: float = 60.0,
                 dialog_after: float = 3.0):
        self.pid, self.log, self.dumps = pid, log, dumps
        self.grace, self.quiet, self.dialog_after = grace, quiet, dialog_after
        self._size = 0
        self._offset = 0
        self._last_growth = time.monotonic()
        self._trouble_at: float | None = None       # When the menu last logged a Hang/Stall line
        self._trouble = ""
        self._progress_at = time.monotonic()        # When a self-test step last started or ended
        self._stacks: list[str] = []
        self._dialog_seen: float | None = None
        self.finished: bool | None = None           # The menu logged the end of the self-test: passed or not
        self.last_line = ""                         # The menu's last log line (for a game that vanished)
        self.popups: list[str] = []                 # Error popups seen and dealt with ("process: title: text")
        # The log is the previous session's until the menu starts a new one: told apart by its "# started" line
        self._old_session = _session_of(log)
        self._session = ""

    # ---- Signals ----------------------------------------------------------------------------------------------------

    def _read_log(self) -> None:
        session = _session_of(self.log)
        if not session or session == self._old_session:
            self._last_growth = time.monotonic()    # This run's log isn't there yet: not silent, just not started
            return
        if session != self._session:    # A new session's log (the menu rotated it): from its start
            self._session = session
            self._offset = self._size = 0
        try:
            size = self.log.stat().st_size
        except OSError:
            return
        if size != self._size:
            self._size = size
            self._last_growth = time.monotonic()
        if size <= self._offset:
            return
        try:
            with self.log.open("rb") as f:
                f.seek(self._offset)
                chunk = f.read(size - self._offset)
        except OSError:
            return
        end = chunk.rfind(b"\n")
        if end < 0:
            return
        self._offset += end + 1
        for raw in chunk[:end].split(b"\n"):
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if line.strip():
                self.last_line = line
            if "] Self-test PASSED: " in line or "] Self-test FAILED: " in line:
                self.finished = "PASSED" in line    # Done: from now on the log going quiet is normal
            if "] Self-test: " in line:
                self._progress_at = time.monotonic()
                self._trouble_at = None         # Progress again: what was logged before is over
                self._stacks.clear()
            elif "Hang: no frame" in line or ("Stall:" in line and "did not come" in line):
                if self._trouble_at is None:
                    self._trouble_at = time.monotonic()
                self._trouble = line.split("] ", 2)[-1]
            elif "Hang: thread" in line or "Stall: thread" in line:
                self._stacks.append(line.split("] ", 2)[-1])
                self._stacks = self._stacks[-24:]

    def check(self, step: str) -> Hang | None:
        """None while the game makes progress; a Hang once it is stuck (call kill afterwards)."""
        self._read_log()
        now = time.monotonic()
        # Windows' own crash popup (WerFault: "MirrorsEdge.exe has stopped working"): noted, then closed, which lets
        # the crashed game end instead of waiting for someone to click
        for popup in close_error_popups() if WINDOWS else []:
            if popup not in self.popups:
                self.popups.append(popup)
        dialog = _dialog_text(self.pid) if WINDOWS else ""
        if dialog:
            self._dialog_seen = self._dialog_seen or now
            if now - self._dialog_seen >= self.dialog_after:
                return Hang("a dialog is up and the game waits for it", step, dialog, list(self._stacks))
        else:
            self._dialog_seen = None
        if self._trouble_at is not None and now - self._trouble_at >= self.grace and now - self._progress_at >= self.grace:
            return Hang(f"{self._trouble} (and no progress {now - self._trouble_at:.0f} s later)", step, "",
                        list(self._stacks))
        if self.finished is not None:
            return None
        if self._size and now - self._last_growth >= self.quiet:
            return Hang(f"the menu's log has been silent for {now - self._last_growth:.0f} s", step, "", list(self._stacks))
        return None

    def record(self, hang: Hang) -> Hang:
        """Writes the minidump (Windows) and returns the hang with it."""
        if WINDOWS:
            self.dumps.mkdir(parents=True, exist_ok=True)
            path = self.dumps / f"hang-{datetime.now():%Y%m%d-%H%M%S}.dmp"
            if _minidump(self.pid, path):
                hang.dump = path
        return hang


def _session_of(log: Path) -> str:
    """The log's "# started <time>" line (which session wrote it), "" if there is none yet."""
    try:
        with log.open("rb") as f:
            for _ in range(4):
                line = f.readline().decode("utf-8", "replace").strip()
                if line.startswith("# started"):
                    return line
    except OSError:
        pass
    return ""


_EXIT_CODES = {
    0xC0000005: "access violation",
    0xC0000409: "fail-fast or stack buffer overrun (skips the menu's crash logger)",
    0xC00000FD: "stack overflow",
    0xC0000374: "heap corruption",
    0xC000041D: "an exception in a callback the system couldn't unwind",
    0x00000003: "abort() (a failed assert or a CRT error)",
    0x0000DEAD: "the failsafe (Ctrl+Alt+Shift+F12)",
    0x40010004: "killed by the debugger or the system",
}


def exit_code_meaning(code: int | None) -> str:
    """What a game exit code means, "" if nothing known."""
    return _EXIT_CODES.get((code or 0) & 0xFFFFFFFF, "")


GAME_EXE = "mirrorsedge.exe"
_WER = ("werfault.exe", "werfaultsecure.exe")


def close_error_popups() -> list[str]:
    """Windows' error popups about the game (WerFault's crash dialog, a "can't start" box): each one's text, after it
    was closed (WerFault is ended, which lets the crashed game go). Windows only; [] elsewhere."""
    if not WINDOWS:
        return []
    found: list[str] = []
    for hwnd, pid, cls, title in _windows():
        exe = _process_name(pid)
        if exe not in _WER and not (exe == GAME_EXE and cls == "#32770" and _is_error_box(hwnd, title)):
            continue
        text = _window_text_all(hwnd)
        if exe in _WER and GAME_EXE.split(".")[0] not in (title + text).lower():
            continue        # A crash popup of another program: not ours to close
        found.append(f"{exe}: {title}: {text}"[:600])
        if exe in _WER:
            _terminate(pid)
    return found


# ---- Windows ------------------------------------------------------------------------------------------------------------

if WINDOWS:
    from ctypes import wintypes

    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    _user32.EnumWindows.argtypes = [_EnumProc, wintypes.LPARAM]
    _user32.EnumChildWindows.argtypes = [wintypes.HWND, _EnumProc, wintypes.LPARAM]
    _user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    _user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    _user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    _user32.IsWindowVisible.argtypes = [wintypes.HWND]
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                     ctypes.POINTER(wintypes.DWORD)]
    _kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def _text(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(1024)
    _user32.GetWindowTextW(hwnd, buf, 1024)
    return buf.value


def _class(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(256)
    _user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def _windows() -> list[tuple[int, int, str, str]]:
    """Visible top-level windows: (hwnd, owner pid, class, title)."""
    out: list[tuple[int, int, str, str]] = []

    @_EnumProc
    def each(hwnd, _):
        if _user32.IsWindowVisible(hwnd):
            owner = wintypes.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            out.append((hwnd, owner.value, _class(hwnd), _text(hwnd)))
        return True

    _user32.EnumWindows(each, 0)
    return out


def _process_name(pid: int) -> str:
    """The executable's file name, lowercase ("" if it can't be read)."""
    handle = _kernel32.OpenProcess(0x1000, False, pid)     # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if not _kernel32.QueryFullProcessImageNameW(wintypes.HANDLE(handle), 0, buf, ctypes.byref(size)):
            return ""
        return buf.value.replace("/", "\\").rsplit("\\", 1)[-1].lower()
    finally:
        _kernel32.CloseHandle(wintypes.HANDLE(handle))


def _terminate(pid: int) -> None:
    handle = _kernel32.OpenProcess(0x0001, False, pid)     # PROCESS_TERMINATE
    if handle:
        _kernel32.TerminateProcess(wintypes.HANDLE(handle), 1)
        _kernel32.CloseHandle(wintypes.HANDLE(handle))


def _window_text_all(hwnd) -> str:
    """The texts of a window's Static children (a dialog's message), joined."""
    parts: list[str] = []

    @_EnumProc
    def child(c, _):
        if _class(c) == "Static" and _text(c).strip():
            parts.append(" ".join(_text(c).split()))
        return True

    _user32.EnumChildWindows(hwnd, child, 0)
    return " / ".join(parts)


def _is_error_box(hwnd, title: str) -> bool:
    """A system error box of the game itself (a missing DLL, an entry point not found): titled like one"""
    t = title.lower()
    return "error" in t or "system error" in t or t.endswith(".exe")


def _dialog_text(pid: int) -> str:
    """"title: text" of a visible dialog (#32770: assert and message boxes) owned by `pid`, or ""."""
    found: list[str] = []

    @_EnumProc
    def top(hwnd, _):
        owner = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not _user32.IsWindowVisible(hwnd) or _class(hwnd) != "#32770":
            return True
        parts: list[str] = []

        @_EnumProc
        def child(c, _):
            if _class(c) == "Static" and _text(c).strip():
                parts.append(" ".join(_text(c).split()))
            return True

        _user32.EnumChildWindows(hwnd, child, 0)
        found.append(f"{_text(hwnd)}: {' / '.join(parts)}"[:600])
        return False

    _user32.EnumWindows(top, 0)
    return found[0] if found else ""


def _minidump(pid: int, path: Path) -> bool:
    """Threads, stacks and modules of `pid` (MiniDumpWithThreadInfo: a few MB), with dbghelp. False on failure."""
    PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
    MINIDUMP = 0x00001000 | 0x00000004 | 0x00000001     # WithThreadInfo | WithHandleData | WithDataSegs
    try:
        dbghelp = ctypes.WinDLL("dbghelp")
        process = _kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not process:
            return False
        try:
            import msvcrt
            with open(path, "wb") as f:
                handle = msvcrt.get_osfhandle(f.fileno())
                ok = dbghelp.MiniDumpWriteDump(wintypes.HANDLE(process), wintypes.DWORD(pid), wintypes.HANDLE(handle),
                                               MINIDUMP, None, None, None)
            if not ok:
                path.unlink(missing_ok=True)
            return bool(ok)
        finally:
            _kernel32.CloseHandle(wintypes.HANDLE(process))
    except OSError:
        return False
