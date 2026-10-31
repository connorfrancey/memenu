"""What `memenu dev-test` needs besides building and launching: watching the game, and reporting how the run went
(the menu's log, Windows' crash record, the crash location resolved through the PDB, the crash dump)."""

from __future__ import annotations

import csv
import ctypes
import io
import json
import os
import subprocess
import time
from ctypes import wintypes
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

GAME_EXE = "MirrorsEdge.exe"


# ---- the game process ----------------------------------------------------------------------------------------------
def game_pids() -> list[int]:
    """Process ids of running MirrorsEdge.exe (tasklist sees elevated processes too)."""
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {GAME_EXE}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, check=False).stdout
    pids = []
    for row in csv.reader(io.StringIO(out)):
        if len(row) > 1 and row[0].lower() == GAME_EXE.lower():
            pids.append(int(row[1]))
    return pids


def wait_for_game_exit(poll: float = 1.0) -> None:
    """Returns once no MirrorsEdge.exe is running (KeyboardInterrupt stops waiting)."""
    while game_pids():
        time.sleep(poll)


# ---- crash records -------------------------------------------------------------------------------------------------
@dataclass
class Crash:
    time: str
    module: str             # Faulting module name
    module_path: str
    code: str               # Exception code, e.g. 0xc0000005
    offset: int             # Fault offset in that module

    @property
    def description(self) -> str:
        return {
            "0xc0000005": "access violation (bad pointer)",
            "0x80000003": "breakpoint: a debug-build check (assert, /RTC run-time check) fired",
            "0xe06d7363": "unhandled C++ exception",
            "0xc0000409": "stack buffer overrun / fail-fast",
            "0xc00000fd": "stack overflow",
            "0xc000001d": "illegal instruction",
        }.get(self.code.lower(), "")


def crashes_since(start: datetime) -> list[Crash]:
    """Windows' Application Error records (event 1000) for the game since `start`, oldest first."""
    script = (
        "$e = Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=[datetime]'"
        + start.strftime("%Y-%m-%dT%H:%M:%S")
        + "'} -ErrorAction SilentlyContinue | Where-Object { $_.Properties[0].Value -eq '" + GAME_EXE + "' };"
        " @($e | ForEach-Object { @{ t = $_.TimeCreated.ToString('HH:mm:ss'); p = @($_.Properties | ForEach-Object"
        " { [string]$_.Value }) } }) | ConvertTo-Json -Depth 3 -Compress"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                         capture_output=True, text=True, check=False).stdout.strip()
    if not out:
        return []
    data = json.loads(out)
    if isinstance(data, dict):
        data = [data]
    crashes = []
    for ev in data:
        p = ev["p"]
        # 0 app, 1 app version, 2 app time stamp, 3 module, 4 module version, 5 module time stamp, 6 exception code,
        # 7 fault offset, 8 pid, 9 start time, 10 app path, 11 module path, ...
        try:   # Both are hex, with or without 0x ("e06d7363", "0x0016bae4")
            code = f"0x{int(p[6], 16) & 0xFFFFFFFF:08x}"
            offset = int(p[7], 16)
        except (ValueError, IndexError):
            continue
        crashes.append(Crash(ev["t"], p[3], p[11] if len(p) > 11 else "", code, offset))
    return list(reversed(crashes))   # Get-WinEvent returns newest first


def dumps_since(start: datetime) -> list[Path]:
    folder = Path(os.environ.get("LOCALAPPDATA", "")) / "CrashDumps"
    if not folder.is_dir():
        return []
    t0 = start.timestamp()
    return sorted((p for p in folder.glob(f"{GAME_EXE}*.dmp") if p.stat().st_mtime >= t0), key=lambda p: p.stat().st_mtime)


# ---- symbols -------------------------------------------------------------------------------------------------------
class _SymbolInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", ctypes.c_char * 1024)]


class _SymbolInfoNative(ctypes.Structure):   # The real SYMBOL_INFO (Name[1]): only used for its size
    _fields_ = _SymbolInfo._fields_[:-1] + [("Name", ctypes.c_char * 1)]


_SYMBOL_INFO_SIZE = ctypes.sizeof(_SymbolInfoNative)


class _Line(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wintypes.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]


def symbolize(dll: Path, rva: int) -> str | None:
    """'function  file:line' for an offset into `dll`, through its PDB (found via the path stored in the DLL)."""
    try:
        dbghelp = ctypes.WinDLL("dbghelp")
    except OSError:
        return None
    proc = ctypes.c_void_p(0x6D656D)   # Any unique value: no live process is needed
    dbghelp.SymSetOptions(0x10 | 0x2)  # SYMOPT_LOAD_LINES | SYMOPT_UNDNAME
    dbghelp.SymInitializeW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, wintypes.BOOL]
    dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
    dbghelp.SymLoadModuleExW.argtypes = [ctypes.c_void_p, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                         ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    dbghelp.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                    ctypes.POINTER(_SymbolInfo)]
    dbghelp.SymGetLineFromAddr64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(wintypes.DWORD),
                                             ctypes.POINTER(_Line)]
    dbghelp.SymCleanup.argtypes = [ctypes.c_void_p]
    if not dbghelp.SymInitializeW(proc, None, False):
        return None
    try:
        base = 0x10000000
        if not dbghelp.SymLoadModuleExW(proc, None, str(dll), None, base, 0x4000000, None, 0):
            return None
        addr = base + rva
        sym = _SymbolInfo()
        sym.SizeOfStruct = _SYMBOL_INFO_SIZE   # Must be sizeof(SYMBOL_INFO) exactly, not counting our bigger Name
        sym.MaxNameLen = 1023
        disp = ctypes.c_uint64()
        name = sym.Name.decode(errors="replace") if dbghelp.SymFromAddr(proc, addr, ctypes.byref(disp),
                                                                         ctypes.byref(sym)) else None
        line = _Line()
        line.SizeOfStruct = ctypes.sizeof(_Line)
        d = wintypes.DWORD()
        where = (f"{line.FileName.decode(errors='replace')}:{line.LineNumber}"
                 if dbghelp.SymGetLineFromAddr64(proc, addr, ctypes.byref(d), ctypes.byref(line)) else None)
        if not name and not where:
            return None
        return "  ".join(x for x in (f"{name}+{disp.value:#x}" if name else None, where) if x)
    finally:
        dbghelp.SymCleanup(proc)


# ---- the menu's own log --------------------------------------------------------------------------------------------
def log_since(dll: Path, start: datetime) -> list[str] | None:
    """logs/memenu.log next to the DLL, if it was written during this run (None: the DLL never started logging).
    Only the message lines: the header (lines starting with '#', see src/log.cpp) is left out."""
    log = dll.parent / "logs" / "memenu.log"
    if not log.exists() or log.stat().st_mtime < start.timestamp() - 1:
        return None
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    return [line for line in lines if line and not line.startswith("#")]
