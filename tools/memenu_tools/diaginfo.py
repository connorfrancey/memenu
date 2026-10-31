"""`memenu --copy-info`: everything needed to diagnose a problem, as one plain-text report on the clipboard.

The report has: the system, git state, toolchain, each build folder (options, features, the last build's number and
magic), the game install(s), and the menu's logs: the current one and the newest archived ones (logs/.old/*.log.zst,
decompressed), each with its header (build number, magic, build details) and the lines that matter.
"""

from __future__ import annotations

import ctypes
import platform
import re
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

from . import build as b
from . import features as feat
from .devtest import crashes_since, game_pids
from .game import LAUNCHER, detected_installs, is_elevated, project_version
from .toolchain import ROOT

TAIL_LINES = 120            # Last lines of each current log
ARCHIVE_TAIL_LINES = 60     # Last lines of each archived log
ARCHIVES_PER_FOLDER = 2     # Newest archived logs per logs/ folder
MAX_PROBLEM_LINES = 80      # warning/error/critical lines kept from anywhere in a log


# ---- zstd: Python 3.14 has it built in, older versions use the zstandard package -----------------------------------
def zstd_decompress(data: bytes) -> bytes:
    try:
        from compression import zstd   # Python 3.14+
        return zstd.decompress(data)
    except ImportError:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)


# ---- clipboard -----------------------------------------------------------------------------------------------------
def copy_to_clipboard(text: str) -> None:
    """Puts `text` on the Windows clipboard as Unicode text. Raises OSError when the clipboard can't be opened."""
    CF_UNICODETEXT = 13
    GMEM_MOVEABLE = 0x0002
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.OpenClipboard.argtypes = [wintypes.HWND]
    user32.OpenClipboard.restype = wintypes.BOOL
    user32.EmptyClipboard.restype = wintypes.BOOL
    user32.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
    user32.SetClipboardData.restype = wintypes.HANDLE
    user32.CloseClipboard.restype = wintypes.BOOL
    kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    kernel32.GlobalAlloc.restype = wintypes.HGLOBAL
    kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalFree.argtypes = [wintypes.HGLOBAL]

    data = text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-16-le") + b"\0\0"
    for _ in range(10):     # Another program may hold the clipboard for a moment
        if user32.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise OSError(f"cannot open the clipboard (error {ctypes.get_last_error()})")
    try:
        user32.EmptyClipboard()
        mem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
        if not mem:
            raise OSError("GlobalAlloc failed")
        ptr = kernel32.GlobalLock(mem)
        ctypes.memmove(ptr, data, len(data))
        kernel32.GlobalUnlock(mem)
        if not user32.SetClipboardData(CF_UNICODETEXT, mem):
            kernel32.GlobalFree(mem)
            raise OSError(f"SetClipboardData failed (error {ctypes.get_last_error()})")
    finally:
        user32.CloseClipboard()


# ---- helpers -------------------------------------------------------------------------------------------------------
def _run(args: list[str], cwd: Path = ROOT) -> str:
    try:
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                           stdin=subprocess.DEVNULL, timeout=20, check=False)
        return (r.stdout + r.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"({args[0]}: {e})"


def _when(path: Path) -> str:
    return datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y-%m-%d %H:%M:%S")


def _section(title: str) -> str:
    return f"\n## {title}\n"


def split_log(text: str) -> tuple[list[str], list[str]]:
    """(header lines, message lines) of a log in the format of src/log.cpp. Older logs have no header."""
    header, body = [], []
    lines = text.splitlines()
    i = 0
    while i < len(lines) and lines[i].startswith("#"):
        header.append(lines[i])
        i += 1
    body = [line for line in lines[i:] if line.strip()]
    return header, body


_PROBLEM = re.compile(r"\[(warning|error|critical)\]")


def _describe_log(name: str, text: str, tail: int) -> str:
    header, body = split_log(text)
    out = [f"### {name}", f"{len(body)} lines" + ("" if header else " (no header: written by an older build)")]
    out += header
    problems = [line for line in body if _PROBLEM.search(line)]
    if problems:
        shown = problems[-MAX_PROBLEM_LINES:]
        out.append(f"-- warnings/errors ({len(problems)}, last {len(shown)}):")
        out += shown
    out.append(f"-- last {min(tail, len(body))} lines:")
    out += body[-tail:]
    if body and not any("Unloading" in line for line in body[-5:]):
        out.append("-- (no 'Unloading' at the end: still running, closed with the game, or crashed)")
    return "\n".join(out)


def _log_folders(game_dirs: list[Path]) -> list[Path]:
    """Every place a medge_menu.dll writes its logs/: the build output folders, the build folders (the launcher
    injects from there) and the menu folder of each game install."""
    folders = [ROOT / "build", ROOT / "build" / "minimal"]
    folders += sorted(p for p in ROOT.glob("cmake-build-*") if p.is_dir())
    folders += [d / "memenu" for d in game_dirs]
    return [f for f in folders if f.exists()]


def _game_dirs(installs: str) -> list[Path]:
    """Binaries folders mentioned in the launcher's --list output."""
    dirs: list[Path] = []
    for m in re.finditer(r"[A-Za-z]:[\\/][^\r\n\"|]+", installs):
        p = Path(m[0].strip().rstrip("."))
        for candidate in (p, p.parent, p / "Binaries"):
            if candidate.is_dir() and (candidate / "MirrorsEdge.exe").exists() and candidate not in dirs:
                dirs.append(candidate)
    return dirs


# ---- the report ----------------------------------------------------------------------------------------------------
def collect() -> str:
    parts = [f"# Mirror's Edge Menu diagnostic info ({datetime.now().strftime('%Y-%m-%d %H:%M:%S')})"]

    parts.append(_section("System"))
    parts.append("\n".join([
        f"OS: {platform.platform()} ({platform.machine()})",
        f"Python: {sys.version.split()[0]}, admin: {'yes' if is_elevated() else 'no'}",
        f"Project version: {project_version()}",
        f"Mirror's Edge running: {', '.join(map(str, game_pids())) or 'no'}",
    ]))

    parts.append(_section("Git"))
    parts.append(_run(["git", "log", "-5", "--format=%h %ad %s", "--date=short"]))
    parts.append("branch: " + _run(["git", "rev-parse", "--abbrev-ref", "HEAD"]))
    status = _run(["git", "status", "--short"]).splitlines()
    parts.append(f"{len(status)} changed file(s)" + (":\n" + "\n".join(status[:40]) if status else ""))

    parts.append(_section("Toolchain"))
    cmake = _run(["cmake", "--version"])
    parts.append(cmake.splitlines()[0] if cmake else "cmake: not on PATH")
    vs = _run([r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe", "-latest", "-property",
               "catalog_productDisplayVersion"])
    parts.append(f"Visual Studio: {vs or '?'}")

    parts.append(_section("Build folders"))
    features = feat.load()
    for config in b.PRESETS:
        folder = b.build_dir(config)
        if not (folder / "CMakeCache.txt").exists():
            continue
        lines = [f"### {folder.name}"]
        lines.append("features: " + feat.summary(features, feat.current_state(config, features)))
        lines.append("optimizations: " + ("on" if b.option_enabled(config, "MEMENU_OPTIMIZATIONS") else "OFF"))
        stage = b.cache_value(config, "MEMENU_MINIMAL_STAGE")
        if b.option_enabled(config, "MEMENU_MINIMAL"):
            lines.append(f"minimal stage: {stage}")
        dll = folder / "medge_menu.dll"
        lines.append(f"medge_menu.dll: {_when(dll)}, {dll.stat().st_size // 1024} KB" if dll.exists()
                     else "medge_menu.dll: not built")
        generated = folder / "generated" / "build_info.cpp"
        if generated.exists():
            for m in re.finditer(r"build_info::k(Number|MagicHex|Id|GitCommit|Timestamp)\s*=\s*\"?([^\";]+)",
                                 generated.read_text(encoding="utf-8", errors="replace")):
                lines.append(f"last build {m[1]}: {m[2]}")
        build_log = folder / "memenu-build.log"
        if build_log.exists():
            errors = [line for line in build_log.read_text(encoding="utf-8", errors="replace").splitlines()
                      if re.search(r"\berror\b", line, re.I)]
            lines.append(f"last build log ({_when(build_log)}): {len(errors)} error line(s)")
            lines += ["  " + line for line in errors[:15]]
        parts.append("\n".join(lines))

    parts.append(_section("Game"))
    launcher = next((p for p in (b.build_dir(c) / LAUNCHER for c in ("debug", "release")) if p.exists()), None)
    try:
        installs = detected_installs(launcher) if launcher else "(launcher not built: install detection unavailable)"
    except OSError as e:
        installs = f"(launcher failed: {e})"
    parts.append(installs or "no install found")
    crashes = crashes_since(datetime.fromtimestamp(time.time() - 3 * 86400))
    # Without admin rights the launcher can't list the installs: also look where crashed copies of the menu were,
    # and in Steam's default library.
    hints = installs + "\n" + "\n".join(str(Path(c.module_path).parent.parent) for c in crashes
                                        if c.module_path.lower().endswith(r"\memenu\medge_menu.dll"))
    hints += "\n" + r"C:\Program Files (x86)\Steam\steamapps\common\mirrors edge\Binaries"
    game_dirs = _game_dirs(hints)
    for d in game_dirs:
        menu = d / "memenu"
        parts.append(f"{d}: dinput8.dll loader {'installed' if (d / 'dinput8.dll').exists() else 'not installed'}, "
                     f"menu DLL {'present' if (menu / 'medge_menu.dll').exists() else 'missing'}"
                     + (", DISABLED (memenu/disabled)" if (menu / 'disabled').exists() else ""))
        loader_log = menu / "loader.log"
        if loader_log.exists():
            parts.append("loader.log (last 15 lines):\n" +
                         "\n".join(loader_log.read_text(encoding="utf-8", errors="replace").splitlines()[-15:]))

    parts.append(_section(f"Windows crash records for Mirror's Edge (last 3 days): {len(crashes)}"))
    for c in crashes[-10:]:
        parts.append(f"{c.time}  {c.code} {c.description}  in {c.module}+{c.offset:#x}  ({c.module_path})")

    parts.append(_section("Logs"))
    found = False
    for folder in _log_folders(game_dirs):
        logs = folder / "logs"
        current = logs / "memenu.log"
        legacy = folder / "memenu.log"   # Builds before the logs/ folder
        for log in (current, legacy):
            if log.exists():
                found = True
                parts.append(_describe_log(f"{log} (written {_when(log)})",
                                           log.read_text(encoding="utf-8-sig", errors="replace"), TAIL_LINES))
        old = logs / ".old"
        archived = sorted(old.glob("*.log.zst"))[-ARCHIVES_PER_FOLDER:] if old.is_dir() else []
        for arc in reversed(archived):
            found = True
            try:
                text = zstd_decompress(arc.read_bytes()).decode("utf-8-sig", errors="replace")
                parts.append(_describe_log(f"{arc} (archived)", text, ARCHIVE_TAIL_LINES))
            except Exception as e:   # noqa: BLE001 - a broken archive must not stop the report
                parts.append(f"### {arc}\n(cannot decompress: {e})")
    if not found:
        parts.append("No memenu logs found: the menu never started in any of these folders.")

    return "\n".join(parts) + "\n"
