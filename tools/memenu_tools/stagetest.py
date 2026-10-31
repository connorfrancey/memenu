"""`memenu verbose-build`: build the staged build one stage at a time and test every stage without the game.

Per stage (cmake/Features.cmake lists them; each adds a layer or a group of features):
  1. configure the staged build folder for that stage, build medge_menu.dll and memenu_testhost.exe
  2. static checks: the build's identity (build_info), the DLL is 32-bit and imports only Windows DLLs (no Visual C++
     runtime DLLs that players may not have), compiler warnings in our code
  3. run 1: the test host loads the DLL into a D3D9 app, draws, opens the menu, resets the device, unloads it
     (see tools/testhost/src/main.cpp); then the menu's log is checked for every milestone and for errors
  4. run 2 in the same folder: the previous log must be archived (logs/.old) and the saved config loaded back
When a stage fails, everything needed to find out why goes into build/verbose-build/<time>/ (report.md is written
for Claude Code: what the stage adds, which check failed, the build diagnostics, the crash resolved through the PDB,
the menu's logs, the DLL's imports, the CMake cache, git state, ...).
"""

from __future__ import annotations

import json
import re
import shutil
import struct
import subprocess
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from rich.console import Console
from rich.markup import escape
from rich.table import Table

from . import build as b
from . import features as feat
from .devtest import symbolize
from .toolchain import ROOT, Toolchain

HOST = "memenu_testhost.exe"
SESSIONS = ROOT / "build" / "verbose-build"

# DLLs the menu may import: they are part of Windows (no runtime installer needed on a player's machine)
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll",
    "d3d9.dll", "dwmapi.dll", "winmm.dll", "imm32.dll", "psapi.dll", "dbghelp.dll", "version.dll", "shlwapi.dll",
    "comdlg32.dll", "ws2_32.dll", "ntdll.dll", "xinput1_4.dll", "xinput9_1_0.dll", "uxtheme.dll", "bcrypt.dll",
}
RUNTIME_DLL = re.compile(r"^(vcruntime|msvcp|ucrtbase|concrt|api-ms-win-crt-)", re.I)

# Log lines that are expected outside the game (the test host is not Mirror's Edge)
EXPECTED_ERRORS = [
    re.compile(p) for p in (
        r"pattern not found",                       # The game's code is not there to scan
        r"Game integration unavailable",
    )
]
EXPECTED_WARNINGS = [
    re.compile(p) for p in (
        r"Game window: none",                       # The host's window is hidden
        r"Player state change hook is not implemented",
        r"TAS unavailable",
        r"Module: .*windhawk|Module: .*overlay",    # Tools the developer runs; logged as possible conflicts
    )
]


# ---- results -------------------------------------------------------------------------------------------------------
@dataclass
class Check:
    name: str
    level: str          # "pass", "warn", "fail"
    detail: str = ""


@dataclass
class StageResult:
    stage: int
    adds: str
    new_features: list[str]
    checks: list[Check] = field(default_factory=list)
    seconds: float = 0.0
    folder: Path | None = None              # This stage's test folder in the session
    build_log: Path | None = None
    diagnostics: list[b.Diagnostic] = field(default_factory=list)
    host_reports: list[dict] = field(default_factory=list)
    logs: list[Path] = field(default_factory=list)
    crash_frames: list[str] = field(default_factory=list)   # Symbolized

    @property
    def failed(self) -> bool:
        return any(c.level == "fail" for c in self.checks)

    @property
    def warnings(self) -> list[Check]:
        return [c for c in self.checks if c.level == "warn"]

    def add(self, name: str, ok: bool, detail: str = "", warn_only: bool = False) -> bool:
        self.checks.append(Check(name, "pass" if ok else ("warn" if warn_only else "fail"), detail))
        return ok


# ---- static checks -------------------------------------------------------------------------------------------------
def pe_info(dll: Path) -> dict:
    """Machine type and imported DLL names, read straight from the PE file (no tools needed)."""
    data = dll.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    nsections = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (96 if magic == 0x10B else 112)
    import_rva = struct.unpack_from("<I", data, dirs + 8)[0]
    sections = []
    sec = opt + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(nsections):
        va, raw_size, raw_ptr = struct.unpack_from("<III", data, sec + i * 40 + 12)
        vsize = struct.unpack_from("<I", data, sec + i * 40 + 8)[0]
        sections.append((va, max(vsize, raw_size), raw_ptr))

    def off(rva: int) -> int | None:
        for va, size, ptr in sections:
            if va <= rva < va + size:
                return rva - va + ptr
        return None

    imports = []
    pos = off(import_rva) if import_rva else None
    while pos is not None:
        name_rva = struct.unpack_from("<I", data, pos + 12)[0]
        if not name_rva:
            break
        npos = off(name_rva)
        if npos is not None:
            imports.append(data[npos:data.index(b"\0", npos)].decode("ascii", "replace"))
        pos += 20
    return {"machine": machine, "imports": imports, "size": len(data)}


def generated_build_info(folder: Path) -> dict[str, str]:
    path = folder / "generated" / "build_info.cpp"
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    return {m[1]: m[2] for m in re.finditer(r"build_info::k(\w+)\s*=\s*\"?([^\";]*)\"?;", text)}


# ---- the test host -------------------------------------------------------------------------------------------------
def run_host(host: Path, dll: Path, run_dir: Path, seconds: float, timeout: float = 180) -> tuple[dict, str, int]:
    """Runs the test host on `dll` (copied with its PDB into `run_dir`, so its logs and config land there).
    Returns (report, console output, exit code). Exit codes: 0 ok, 1 a phase failed, 2 setup, 3 crash, 4 hang."""
    run_dir.mkdir(parents=True, exist_ok=True)
    target = run_dir / dll.name
    if not target.exists() or target.stat().st_mtime < dll.stat().st_mtime:
        shutil.copy2(dll, target)
        pdb = dll.with_suffix(".pdb")
        if pdb.exists():
            shutil.copy2(pdb, run_dir / pdb.name)
    report = run_dir / f"host-{int(time.time() * 1000)}.json"
    args = [str(host), "--dll", str(target), "--report", str(report), "--dumps", str(run_dir / "dumps"),
            "--seconds", str(seconds)]
    try:
        proc = subprocess.run(args, cwd=run_dir, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=timeout, check=False)
        output, code = proc.stdout + proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as e:
        subprocess.run(["taskkill", "/F", "/T", "/IM", HOST], capture_output=True, check=False)
        output, code = ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")), 4
        output += f"\n(memenu: the test host did not finish within {timeout:.0f} s and was killed)"
    data: dict = {}
    if report.exists():
        try:
            data = json.loads(report.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            data = {"result": "unreadable report", "error": str(e)}
    data["exit_code"] = code
    data["report_file"] = str(report)
    return data, output, code


def check_host(result: StageResult, report: dict, run: str, dll: Path) -> None:
    """Checks from the host's own report: every phase, crashes, hangs, leftover threads/handles."""
    outcome = report.get("result", "no report")
    phases = report.get("phases", [])
    for p in phases:
        if not p.get("ok"):
            result.add(f"{run}: phase {p['name']}", False, p.get("detail", ""))
    if outcome == "crash":
        c = report.get("crash", {})
        detail = f"exception {c.get('code')} at {c.get('address')} during phase '{report.get('phase')}'"
        if c.get("access"):
            detail += f" ({c['access']})"
        if c.get("module", "").lower().endswith("medge_menu.dll"):
            where = symbolize(dll, int(c.get("offset", 0)))
            if where:
                detail += f" -> {where}"
                result.crash_frames.append(f"{c.get('address')}  {where}")
        result.add(f"{run}: no crash", False, detail + (f"; dump {c['dump']}" if c.get("dump") else ""))
    elif outcome == "hang":
        c = report.get("crash", {})
        result.add(f"{run}: no hang", False, f"phase '{report.get('phase')}' ran longer than {c.get('limit_ms')} ms; "
                                             f"dump {c.get('dump') or '(none)'}")
    elif outcome != "ok":
        if not any(c.level == "fail" for c in result.checks):
            result.add(f"{run}: test host", False, f"result '{outcome}', exit code {report.get('exit_code')}, last phase "
                                                   f"'{report.get('phase')}'")
    else:
        result.add(f"{run}: every phase passed", True, ", ".join(f"{p['name']} {p['ms']:.0f} ms" for p in phases))

    m = report.get("metrics", {})
    if m:
        threads = m.get("threads_after", 0) - m.get("threads_before", 0)
        handles = m.get("handles_after", 0) - m.get("handles_before", 0)
        memory = (m.get("private_kb_after", 0) - m.get("private_kb_before", 0)) // 1024
        result.add(f"{run}: cleans up after unloading", threads <= 4 and handles <= 200 and memory <= 128,
                   f"threads {threads:+d}, handles {handles:+d}, private memory {memory:+d} MB (the D3D driver keeps "
                   "some after a device reset)", warn_only=True)
    f = report.get("frames", {})
    if f.get("baseline_ms"):
        result.add(f"{run}: frame time", True, f"baseline {f['baseline_ms']:.1f} ms, menu closed "
                                               f"{f.get('menu_closed_ms', 0):.1f} ms, open {f.get('menu_open_ms', 0):.1f} ms")


def check_log(result: StageResult, log: Path, run: str, stage: int, info: dict[str, str], second_run: bool,
              dll: Path) -> None:
    """Checks the menu's log of one run: its header, every milestone of the run, and no unexpected errors."""
    if not result.add(f"{run}: log written", log.exists(), str(log)):
        return
    text = log.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    header = {m[1]: m[2] for m in re.finditer(r"^# (\w+)\s+(.*)$", text, re.M)}
    if info:
        result.add(f"{run}: log header matches the build", header.get("build") == info.get("Number") and
                   header.get("magic") == info.get("MagicHex"),
                   f"header build {header.get('build')} magic {header.get('magic')}, built "
                   f"{info.get('Number')} / {info.get('MagicHex')}")

    def has(pattern: str) -> bool:
        return any(re.search(pattern, line) for line in lines)

    milestones = [
        ("hooks installed", r"Hooks installed"),
        ("EndScene hook runs", r"First EndScene call"),
        ("ImGui initialised", r"ImGui initialised"),
        ("menu opened and closed", r"Menu opened"),
        ("survives a device reset", r"Device Reset to 1024x768 .*D3D_OK"),
        ("unload requested", r"Unload requested|Minimal build attached"),
        ("hooks removed", r"Hooks removed"),
    ]
    if stage >= 3:
        milestones += [("config saved", r"Config saved"), ("startup finished", r"Startup finished")]
    if second_run:
        milestones.append(("previous log archived", r"Previous log archived"))
        if stage >= 3:
            milestones.append(("saved config loaded", r"Config loaded"))
    for name, pattern in milestones:
        result.add(f"{run}: {name}", has(pattern), "" if has(pattern) else f"no line matching /{pattern}/")

    body = [line for line in lines if line and not line.startswith("#")]
    result.add(f"{run}: log ends with 'Unloading'", bool(body) and "Unloading" in body[-1],
               body[-1] if body else "(empty)")

    if stage >= 3:
        m = next((re.search(r"(\d+) modules in (\d+) tabs", line) for line in lines if "modules in" in line), None)
        expected = [f for f in feat.load() if f.stage <= stage]
        if m:
            count = int(m[1])
            result.add(f"{run}: modules registered", (count > 0) == bool(expected),
                       f"{count} modules in {m[2]} tabs for {len(expected)} feature(s) of this stage")

    critical = [line for line in body if "[critical]" in line]
    errors = [line for line in body if "[error]" in line and not any(p.search(line) for p in EXPECTED_ERRORS)]
    warnings = [line for line in body if "[warning]" in line and not any(p.search(line) for p in EXPECTED_WARNINGS)]
    result.add(f"{run}: no critical lines (crash, failed debug check)", not critical, "\n".join(critical[:20]))
    result.add(f"{run}: no unexpected errors", not errors, "\n".join(errors[:20]))
    result.add(f"{run}: no unexpected warnings", not warnings, "\n".join(warnings[:20]), warn_only=True)
    for line in critical:   # The crash logger's call stack: resolve every frame in the menu through the PDB
        for off in re.findall(r"medge_menu\.dll\+0x([0-9A-Fa-f]+)", line):
            where = symbolize(dll, int(off, 16))
            result.crash_frames.append(f"medge_menu.dll+0x{off}  {where or '(no symbol)'}")


# ---- locked build files --------------------------------------------------------------------------------------------
LOCKED = re.compile(r"Permission denied|failed recompaction|being used by another process|LNK1168|LNK1104|"
                    r"cannot open .* for writing", re.I)
LOCK_CHECK = "build folder not locked by another program"


def _locked(log: Path) -> bool:
    return log.exists() and bool(LOCKED.search(_tail(log, 80)))


def lock_holders(folder: Path) -> str:
    """Which programs hold the build folder's files open (ninja's state, the DLL): the usual IDE/antivirus suspects."""
    from .locks import holders
    found: dict[int, str] = {}
    for f in (".ninja_log", ".ninja_deps", "build.ninja", "medge_menu.dll", "medge_menu.pdb", HOST):
        path = folder / f
        if path.exists():
            try:
                found.update(dict(holders(path)))
            except OSError:
                pass
    return ", ".join(f"{name} (pid {pid})" for pid, name in found.items()) or "nothing holds them now (it was brief: antivirus?)"


def _retry_if_locked(console: Console, run, what: str):
    """Runs `run()`; if it failed because a file was locked, waits and runs it once more."""
    r = run()
    if r.returncode != 0 and _locked(r.log):
        console.print(f"  [yellow]{what}: a file in the build folder was locked, retrying in 3 s[/]")
        time.sleep(3)
        r = run()
    return r


# ---- one stage -----------------------------------------------------------------------------------------------------
def test_stage(console: Console, tc: Toolchain, config: str, stage: int, adds: str, session: Path, seconds: float,
               verbose: bool = False) -> StageResult:
    folder_key = b.MINIMAL[config]
    folder = b.build_dir(folder_key)
    features = feat.load()
    result = StageResult(stage, adds, [f.name for f in features if f.stage == stage])
    result.folder = session / f"stage-{stage:02d}"
    t0 = time.time()

    # 1. configure + build
    defines = ["MEMENU_BUILD_TESTHOST=ON", f"MEMENU_MINIMAL_STAGE={stage}"]
    defines = [d for d in defines if b.cache_value(folder_key, d.split("=")[0]) != d.split("=")[1]]
    if defines or not b.is_configured(folder_key):
        r = _retry_if_locked(console, lambda: b.configure(console, tc, folder_key, verbose=verbose,
                                                          defines=tuple(defines)), "configure")
        if r.returncode != 0 and _locked(r.log):
            result.add(LOCK_CHECK, False, f"a file in {folder} stayed locked, so this is not the stage's code. Held by: "
                                          f"{lock_holders(folder)}. Close it (an IDE that loaded this CMake preset, a "
                                          f"debugger) and rerun with --from {stage}.")
        if not result.add("configure", r.returncode == 0, str(r.log)):
            result.build_log = r.log
            result.seconds = time.time() - t0
            return result
    r = _retry_if_locked(console, lambda: b.build(console, tc, folder_key, ("medge_menu", "memenu_testhost"),
                                                  verbose=verbose), "build")
    if r.returncode != 0 and _locked(r.log):
        result.add(LOCK_CHECK, False, f"a file in {folder} stayed locked, so this is not the stage's code. Held by: "
                                      f"{lock_holders(folder)}. Close it and rerun with --from {stage}.")
    result.build_log, result.diagnostics = r.log, r.diagnostics
    errors = [d for d in r.diagnostics if d.kind == "error"]
    if not result.add("build", r.returncode == 0,
                      "; ".join(f"{d.code} {d.location}: {d.message}" for d in errors[:10]) or f"exit {r.returncode}"):
        result.seconds = time.time() - t0
        return result
    ours = [d for d in r.diagnostics if d.kind == "warning" and d.ours]
    result.add("no compiler warnings in our code", not ours,
               "; ".join(f"{d.code} {d.location}: {d.message}" for d in ours[:10]), warn_only=True)

    # 2. static checks
    dll, host = folder / "medge_menu.dll", folder / HOST
    info = generated_build_info(folder)
    result.add("build identity stamped for this stage", f"stage={stage}" in info.get("Options", ""),
               f"build #{info.get('Number')} magic {info.get('MagicHex')} options '{info.get('Options')}'")
    pe = pe_info(dll)
    result.add("DLL is 32-bit (x86)", pe["machine"] == 0x14C, f"machine 0x{pe['machine']:X}")
    runtime = [d for d in pe["imports"] if RUNTIME_DLL.match(d)]
    result.add("no Visual C++ runtime DLLs imported", not runtime,
               f"imports {', '.join(runtime)}: players would need the VC++ runtime (the build should use the static "
               "CRT, /MT)" if runtime else ", ".join(pe["imports"]))
    other = [d for d in pe["imports"] if d.lower() not in SYSTEM_DLLS and not RUNTIME_DLL.match(d)]
    result.add("imports only Windows DLLs", not other, ", ".join(other), warn_only=True)

    # 3./4. two runs in the test host
    if result.folder.exists():
        shutil.rmtree(result.folder, ignore_errors=True)
    for n in (1, 2):
        run = f"run {n}"
        report, output, _code = run_host(host, dll, result.folder, seconds)
        report["output"] = output
        result.host_reports.append(report)
        (result.folder / f"host-run{n}.txt").write_text(output, encoding="utf-8")
        check_host(result, report, run, result.folder / dll.name)
        log = result.folder / "logs" / "memenu.log"
        if stage >= 1:
            if log.exists():
                kept = result.folder / f"memenu-run{n}.log"
                shutil.copy2(log, kept)
                result.logs.append(kept)
            check_log(result, log, run, stage, info, n == 2, result.folder / dll.name)
        if result.failed or stage < 1:   # Stage 0 writes no log: one run says it all
            break
    result.seconds = time.time() - t0
    return result


def print_stage(console: Console, r: StageResult) -> None:
    status = "[bold red]FAILED[/]" if r.failed else ("[yellow]passed with warnings[/]" if r.warnings else "[green]passed[/]")
    console.print(f"  Stage {r.stage} {status} [dim]({r.seconds:.0f} s)[/]")
    for c in r.checks:
        if c.level == "pass":
            continue
        style = "red" if c.level == "fail" else "yellow"
        first, *rest = (c.detail or "").splitlines() or [""]
        console.print(f"    [{style}]{'x' if c.level == 'fail' else '!'} {escape(c.name)}[/]"
                      + (f": {escape(first)}" if first else ""))
        for line in rest[:5]:
            console.print(f"      [dim]{escape(line)}[/]")
    for frame in r.crash_frames[:12]:
        console.print(f"    [red]{escape(frame)}[/]")


def summary_table(results: list[StageResult]) -> Table:
    table = Table(title="verbose-build", show_lines=False)
    table.add_column("Stage", justify="right")
    table.add_column("Adds")
    table.add_column("Result")
    table.add_column("Checks", justify="right")
    table.add_column("Time", justify="right")
    for r in results:
        res = "[red]FAILED[/]" if r.failed else ("[yellow]warnings[/]" if r.warnings else "[green]ok[/]")
        passed = sum(c.level == "pass" for c in r.checks)
        table.add_row(str(r.stage), escape(r.adds[:70]), res, f"{passed}/{len(r.checks)}", f"{r.seconds:.0f} s")
    return table


# ---- the debug bundle ----------------------------------------------------------------------------------------------
def _git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       check=False)
    return (r.stdout + r.stderr).strip()


def _fence(text: str, lang: str = "") -> str:
    return f"```{lang}\n{text.rstrip()}\n```\n"


def _tail(path: Path, n: int) -> str:
    if not path or not path.exists():
        return "(missing)"
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(([f"... ({len(lines) - n} earlier lines)"] if len(lines) > n else []) + lines[-n:])


def stage_sources(stage: int) -> list[str]:
    """Source files of the features this stage adds (from their memenu_feature() FILES)."""
    text = (ROOT / "cmake" / "Features.cmake").read_text(encoding="utf-8")
    files: list[str] = []
    for m in re.finditer(r"^memenu_feature\((\w+)[^\n]*?STAGE\s+(\d+)\s+FILES\s+([^)]*)\)", text, re.M):
        if int(m[2]) == stage:
            files += [f"src/{f}" for f in m[3].split() if f"src/{f}" not in files]
    return files


def write_report(session: Path, config: str, results: list[StageResult], unit_tests: tuple[bool, str] | None,
                 argv: str) -> Path:
    """report.md in the session folder: a summary, and for a failure everything that helps find the cause."""
    failed = next((r for r in results if r.failed), None)
    stages = dict(feat.stages())
    out: list[str] = ["# memenu verbose-build report\n"]
    head = _git("rev-parse", "--short", "HEAD")
    dirty = bool(_git("status", "--porcelain", "--untracked-files=no"))
    out.append(f"- Command: `{argv}`\n- When: {datetime.now():%Y-%m-%d %H:%M:%S}\n- Configuration: {config} "
               f"(staged build folder `{b.build_dir(b.MINIMAL[config]).relative_to(ROOT)}`)\n"
               f"- Commit: {head}{' + uncommitted changes' if dirty else ''} on "
               f"{_git('rev-parse', '--abbrev-ref', 'HEAD')}\n")
    if unit_tests is not None:
        out.append(f"- Unit tests: {'passed' if unit_tests[0] else 'FAILED'}\n")
    locked = failed is not None and any(c.name == LOCK_CHECK and c.level == "fail" for c in failed.checks)
    if locked:
        out.append(f"\n**Result: stage {failed.stage} could not be built: a file in the build folder was locked by "
                   "another program.** That is the environment, not the code: see *Failed checks* for who held it.\n")
    elif failed:
        out.append(f"\n**Result: FAILED at stage {failed.stage}** ({failed.adds}). Stages before it passed, so the cause "
                   f"is most likely in what stage {failed.stage} adds (below).\n")
        out.append("\n## For Claude Code\n\nThis file is the whole picture of the failure: what the failing stage adds, "
                   "the checks that failed, build diagnostics, the crash resolved to source lines, the menu's logs, and "
                   "the build setup. Find the root cause in the source files listed under *What this stage adds*, fix "
                   "it, then rerun `uv run memenu verbose-build --from "
                   f"{failed.stage}` to confirm. The test host is `tools/testhost/src/main.cpp`; the checks are in "
                   "`tools/memenu_tools/stagetest.py`.\n")
    elif unit_tests is not None and not unit_tests[0]:
        out.append("\n**Result: the unit tests FAILED** (stages were not built).\n")
    else:
        out.append("\n**Result: every stage passed.**\n")

    out.append("\n## Stages\n\n| Stage | Adds | Result | Checks passed | Time |\n|---|---|---|---|---|\n")
    for r in results:
        res = "FAILED" if r.failed else ("warnings" if r.warnings else "ok")
        out.append(f"| {r.stage} | {r.adds} | {res} | {sum(c.level == 'pass' for c in r.checks)}/{len(r.checks)} | "
                   f"{r.seconds:.0f} s |\n")
    for r in results:
        if r.warnings and not r.failed:
            out.append(f"\nStage {r.stage} warnings:\n" + "".join(f"- {c.name}: {c.detail}\n" for c in r.warnings))

    if unit_tests is not None and not unit_tests[0]:
        out.append("\n## Unit test output\n\n" + _fence(unit_tests[1][-8000:]))

    if failed:
        r = failed
        out.append(f"\n## Failure: stage {r.stage}\n")
        out.append("\n### What this stage adds\n\n" + f"{r.adds}\n")
        prev = r.stage - 1
        if prev >= 0:
            out.append(f"\nPrevious stage ({prev}): {stages.get(prev, '?')}\n")
        fs = feat.load()
        added = [f for f in fs if f.stage == r.stage]
        if added:
            out.append("\nFeatures added: " + ", ".join(f"`{f.name}` ({f.category}{', needs the game' if f.game else ''})"
                                                       for f in added) + "\n")
        srcs = stage_sources(r.stage)
        if srcs:
            out.append("\nTheir source files:\n" + "".join(f"- `{s}`\n" for s in srcs))
        elif r.stage <= 3:
            layer = {0: "`src/hooks.cpp`, `src/minimal/minimal.cpp`, MinHook/ImGui (cmake/Libraries.cmake)",
                     1: "`src/log.cpp`, `src/log.h`, spdlog, zstd", 2: "`src/theme.cpp`, `src/theme_backend.cpp`, "
                     "nlohmann_json, imjson", 3: "`src/dllmain.cpp`, `src/gui.cpp`, `src/config.cpp`, `src/core/*`, "
                     "`src/diag.cpp`"}.get(r.stage, "")
            out.append(f"\nLayer sources: {layer}\n")

        out.append("\n### Failed checks\n\n" + "".join(
            f"- **{c.name}**" + (f"\n\n{_fence(c.detail)}" if "\n" in c.detail else f": {c.detail}\n")
            for c in r.checks if c.level == "fail"))
        if r.warnings:
            out.append("\n### Warnings\n\n" + "".join(f"- {c.name}: {c.detail}\n" for c in r.warnings))
        if r.crash_frames:
            out.append("\n### Crash location (resolved through the PDB)\n\n" + _fence("\n".join(r.crash_frames)))

        diags = [d for d in r.diagnostics if d.kind == "error" or d.ours]
        if diags:
            out.append("\n### Build diagnostics (errors, and warnings in our code)\n\n" + _fence(
                "\n".join(f"{d.kind} {d.code} {d.location}: {d.message}" for d in diags[:80])))
        if any(c.name in ("build", "configure") for c in r.checks if c.level == "fail"):
            out.append(f"\n### Build log (`{r.build_log}`, end)\n\n" + _fence(_tail(r.build_log, 150)))
            cfg = b.build_dir(b.MINIMAL[config]) / "memenu-configure.log"
            out.append("\n### Configure log (end)\n\n" + _fence(_tail(cfg, 60)))

        for i, rep in enumerate(r.host_reports, 1):
            shown = {k: v for k, v in rep.items() if k != "output"}
            out.append(f"\n### Test host, run {i}\n\n" + _fence(json.dumps(shown, indent=2), "json"))
            if rep.get("output"):
                out.append("Console output:\n\n" + _fence(rep["output"][-4000:]))
        for log in r.logs:
            text = log.read_text(encoding="utf-8", errors="replace").splitlines()
            out.append(f"\n### Menu log `{log.name}` ({len(text)} lines)\n\n" + _fence("\n".join(text[-500:])))

        folder = b.build_dir(b.MINIMAL[config])
        dll = folder / "medge_menu.dll"
        if dll.exists():
            pe = pe_info(dll)
            out.append(f"\n### The DLL\n\n`{dll}`: {pe['size'] // 1024} KB, machine 0x{pe['machine']:X}, imports: "
                       + ", ".join(pe["imports"]) + "\n")
        gen = folder / "generated" / "build_info.cpp"
        if gen.exists():
            out.append("\n### build_info.cpp (this build's identity)\n\n" + _fence(gen.read_text(encoding="utf-8"), "cpp"))
        cache = folder / "CMakeCache.txt"
        if cache.exists():
            keep = [line for line in cache.read_text(encoding="utf-8", errors="replace").splitlines()
                    if re.match(r"(MEMENU_|CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER:|CMAKE_CXX_FLAGS|CMAKE_MSVC_RUNTIME|"
                                r"CMAKE_GENERATOR:)", line)]
            out.append("\n### CMake cache (relevant entries)\n\n" + _fence("\n".join(keep)))

        out.append("\n### Git\n\n" + _fence(_git("status", "--short")) + "\n" +
                   _fence(_git("diff", "--stat", "HEAD")))
        srcs = stage_sources(r.stage)
        if srcs:
            diff = _git("diff", "HEAD", "--", *srcs)
            if diff:
                out.append("\nUncommitted changes to this stage's sources:\n\n" + _fence(diff[:20000], "diff"))
        out.append("\n### Environment\n\nSee `diag-info.txt` in this folder (system, toolchain, all build folders, game "
                   "install, crash records, every menu log): the output of `memenu --copy-info`.\n")

    out.append("\n## Files\n\n" + "".join(
        f"- `{p.relative_to(session)}`\n" for p in sorted(session.rglob("*")) if p.is_file() and p.suffix != ".md"))
    path = session / "report.md"
    path.write_text("".join(out), encoding="utf-8")
    return path
