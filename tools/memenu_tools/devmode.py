"""Developer mode: set the environment variable MEMENU_DEV=1 (or true/yes/on).

What changes:
  - errors in the tool show a full traceback with local variables (rich), instead of a short message
  - every command prints how long it took
  - `verbose-build` keeps every stage's DLL/PDB copies and test folders (like --keep)
  - the interactive menu shows a [DEV] banner and the developer tools
  - `memenu dev ...`: developer tools (hidden otherwise):
      dev env              tool, toolchain, build folders (build numbers, magic), MEMENU_* variables
      dev symbolize ADDR   medge_menu.dll+0x1234 (or a log file with --log) -> function and source line via the PDB
      dev logs             list the menu's logs (current and archived) everywhere; show one, decompressed
      dev crashes          Windows crash records for the game and the test host, resolved through the PDB
      dev pe DLL           a DLL's machine, size and imports, with the checks verbose-build does
      dev host             load a DLL into the test host once (no game) and check the run
      dev unload           ask the menu in a running game (or test host) to unload, like END
"""

from __future__ import annotations

import ctypes
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import click
from rich.console import Console
from rich.markup import escape
from rich.table import Table

TRUE = {"1", "true", "yes", "on"}


def enabled() -> bool:
    return os.environ.get("MEMENU_DEV", "").strip().lower() in TRUE


def setup(console: Console) -> None:
    """Called once at startup when developer mode is on."""
    from rich.traceback import install
    install(console=Console(stderr=True), show_locals=True, width=140, suppress=[click])
    started = time.time()
    import atexit
    atexit.register(lambda: Console(stderr=True).print(f"[dim]\\[dev] {' '.join(sys.argv[1:]) or 'menu'}: "
                                                       f"{time.time() - started:.1f} s[/]"))


def register(cli: click.Group, console: Console) -> None:
    from . import build as b
    from .toolchain import ROOT

    @cli.group("dev", hidden=not enabled())
    def dev() -> None:
        """Developer tools (developer mode: MEMENU_DEV=1)."""
        if not enabled():
            console.print("[yellow]The developer tools need developer mode:[/] set MEMENU_DEV=1 "
                          "(PowerShell: $env:MEMENU_DEV=1, cmd: set MEMENU_DEV=1).")
            sys.exit(2)

    def default_dll(config: str) -> Path:
        candidates = [b.build_dir(config) / "medge_menu.dll", b.build_dir(b.MINIMAL[config]) / "medge_menu.dll"]
        existing = [p for p in candidates if p.exists()]
        if not existing:
            raise click.UsageError(f"no medge_menu.dll built for {config}: pass --dll")
        return max(existing, key=lambda p: p.stat().st_mtime)   # The one built last

    config_opt = click.option("-c", "--config", type=click.Choice(sorted(b.CONFIGS)), default="debug",
                              show_default=True, help="Which build's DLL (the newer of the normal and staged build).")

    # ---- env ------------------------------------------------------------------------------------------------------
    @dev.command("env")
    def env_cmd() -> None:
        """The tool, toolchain and build folders as the tool sees them."""
        from .stagetest import generated_build_info
        from .toolchain import find_toolchain
        t = Table(show_header=False, box=None)
        t.add_column(style="bold")
        t.add_column()
        t.add_row("Repository", str(ROOT))
        t.add_row("Python", f"{sys.version.split()[0]} ({sys.executable})")
        try:
            tc = find_toolchain()
            t.add_row("Visual Studio", str(tc.vs_path))
            t.add_row("MSVC", tc.env.get("VCToolsVersion", "?"))
            t.add_row("CMake / Ninja", f"{tc.cmake} / {tc.ninja}")
        except Exception as e:   # noqa: BLE001
            t.add_row("Toolchain", f"[red]{escape(str(e))}[/]")
        counter = ROOT / "build" / ".build-number"
        t.add_row("Last build number", counter.read_text().strip() if counter.exists() else "-")
        for key in b.PRESETS:
            folder = b.build_dir(key)
            if not (folder / "CMakeCache.txt").exists():
                continue
            info = generated_build_info(folder)
            stage = b.cache_value(key, "MEMENU_MINIMAL_STAGE") if key.startswith("minimal") else None
            t.add_row(folder.name, f"build #{info.get('Number', '-')} magic {info.get('MagicHex', '-')} "
                                   f"{info.get('Options', '')}" + (f" stage {stage}" if stage else ""))
        for k, v in sorted(os.environ.items()):
            if k.startswith("MEMENU_"):
                t.add_row(k, v)
        console.print(t)

    # ---- symbolize --------------------------------------------------------------------------------------------------
    @dev.command("symbolize")
    @click.argument("addresses", nargs=-1)
    @click.option("--dll", type=click.Path(exists=True, dir_okay=False, path_type=Path), help="The DLL (its PDB next to it).")
    @click.option("--log", "log_file", type=click.Path(exists=True, dir_okay=False, path_type=Path),
                  help="Resolve every medge_menu.dll+0x... in this file (a memenu.log, a crash report).")
    @config_opt
    def symbolize_cmd(addresses: tuple[str, ...], dll: Path | None, log_file: Path | None, config: str) -> None:
        """Resolve medge_menu.dll offsets to function and source line (medge_menu.dll+0x1A2B, 0x1A2B or 1A2B).

        Use the exact DLL that crashed (its PDB must be next to it): a rebuilt DLL has different offsets."""
        from .devtest import symbolize
        dll = dll or default_dll(config)
        found = list(addresses)
        if log_file:
            found += re.findall(r"medge_menu\.dll\+0x[0-9A-Fa-f]+", log_file.read_text(encoding="utf-8", errors="replace"))
        if not found:
            raise click.UsageError("give addresses or --log FILE")
        console.print(f"[dim]{dll}[/]")
        for a in dict.fromkeys(found):
            m = re.search(r"(?:0x)?([0-9A-Fa-f]+)$", a)
            if not m:
                console.print(f"  {escape(a)}: not an address")
                continue
            where = symbolize(dll, int(m[1], 16))
            console.print(f"  {escape(a)}  ->  {escape(where) if where else '[yellow](no symbol: wrong DLL/PDB?)[/]'}")

    # ---- logs -------------------------------------------------------------------------------------------------------
    @dev.command("logs")
    @click.argument("which", required=False)
    @click.option("-n", "--tail", default=0, help="Only the last N lines.")
    def logs_cmd(which: str | None, tail: int) -> None:
        """List every menu log (current and archived in logs/.old), or show one: by its number in the list, or a path.

        Archived logs (.log.zst) are decompressed."""
        from .diaginfo import _log_folders, zstd_decompress
        logs: list[Path] = []
        for folder in _log_folders([]):
            logs += [p for p in (folder / "logs" / "memenu.log", folder / "memenu.log") if p.exists()]
            old = folder / "logs" / ".old"
            if old.is_dir():
                logs += sorted(old.glob("*.log.zst"), reverse=True)
        logs.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        if which is None:
            t = Table("#", "Written", "Size", "Log")
            for i, p in enumerate(logs, 1):
                t.add_row(str(i), datetime.fromtimestamp(p.stat().st_mtime).strftime("%m-%d %H:%M:%S"),
                          f"{p.stat().st_size // 1024} KB", escape(str(p.relative_to(ROOT)) if ROOT in p.parents else str(p)))
            console.print(t)
            console.print("[dim]memenu dev logs <#> shows one[/]")
            return
        path = logs[int(which) - 1] if which.isdigit() and 0 < int(which) <= len(logs) else Path(which)
        if not path.exists():
            raise click.BadParameter(f"no log {which}")
        text = (zstd_decompress(path.read_bytes()).decode("utf-8-sig", errors="replace") if path.suffix == ".zst"
                else path.read_text(encoding="utf-8-sig", errors="replace"))
        lines = text.splitlines()[-tail:] if tail else text.splitlines()
        console.print(f"[dim]{path}[/]")
        for line in lines:
            style = ("red" if "[error]" in line or "[critical]" in line else "yellow" if "[warning]" in line
                     else "cyan" if line.startswith("#") else "")
            console.print(f"[{style}]{escape(line)}[/]" if style else escape(line), highlight=False)

    # ---- crashes ----------------------------------------------------------------------------------------------------
    @dev.command("crashes")
    @click.option("--days", default=3, show_default=True)
    def crashes_cmd(days: int) -> None:
        """Windows crash records for Mirror's Edge, resolved through the PDB when that DLL still exists."""
        from .devtest import crashes_since, symbolize
        crashes = crashes_since(datetime.fromtimestamp(time.time() - days * 86400))
        if not crashes:
            console.print(f"No crashes in the last {days} day(s).")
            return
        for c in crashes:
            where = ""
            dll = Path(c.module_path)
            if c.module.lower() == "medge_menu.dll" and dll.exists():
                where = symbolize(dll, c.offset) or ""
            console.print(f"{c.time}  [red]{c.code}[/] {escape(c.description)}  {escape(c.module)}+{c.offset:#x}"
                          + (f"\n    -> {escape(where)}" if where else f"  [dim]{escape(c.module_path)}[/]"))
        console.print("[dim]Offsets only match the DLL that crashed: if it was rebuilt since, the symbols are wrong.[/]")

    # ---- pe ---------------------------------------------------------------------------------------------------------
    @dev.command("pe")
    @click.argument("dll", required=False, type=click.Path(exists=True, dir_okay=False, path_type=Path))
    @config_opt
    def pe_cmd(dll: Path | None, config: str) -> None:
        """A DLL's machine, size and imports, checked like verbose-build does."""
        from .stagetest import RUNTIME_DLL, SYSTEM_DLLS, pe_info
        dll = dll or default_dll(config)
        info = pe_info(dll)
        console.print(f"[bold]{dll}[/]\n  {info['size'] // 1024} KB, machine 0x{info['machine']:X} "
                      f"({'x86, OK' if info['machine'] == 0x14C else '[red]not x86: the game is 32-bit[/]'})")
        for d in info["imports"]:
            note = ("[red]Visual C++ runtime: players may not have it[/]" if RUNTIME_DLL.match(d) else
                    "" if d.lower() in SYSTEM_DLLS else "[yellow]not a known Windows DLL[/]")
            console.print(f"  imports {d}  {note}")

    # ---- host -------------------------------------------------------------------------------------------------------
    @dev.command("host")
    @click.option("--dll", type=click.Path(exists=True, dir_okay=False, path_type=Path))
    @click.option("--seconds", default=2.0, show_default=True)
    @config_opt
    def host_cmd(dll: Path | None, seconds: float, config: str) -> None:
        """Load a DLL into the test host once (no game): hook, draw, menu, reset, unload. Shows the checks and the log."""
        from . import stagetest as st
        dll = dll or default_dll(config)
        hosts = [p / st.HOST for p in (dll.parent, b.build_dir(b.MINIMAL[config]), b.build_dir(config))]
        host = next((h for h in hosts if h.exists()), None)
        if not host:
            raise click.UsageError("memenu_testhost.exe is not built: run `memenu verbose-build` once, or build with "
                                   "`memenu build --with testhost`")
        run_dir = ROOT / "build" / "dev-host" / datetime.now().strftime("%Y%m%d-%H%M%S")
        console.print(f"[dim]{dll} in {host.name}, folder {run_dir}[/]")
        report, output, code = st.run_host(host, dll, run_dir, seconds)
        console.print(escape(output.strip()))
        r = st.StageResult(-1, "dev host", [])
        st.check_host(r, report, "run", run_dir / dll.name)
        log = run_dir / "logs" / "memenu.log"
        if log.exists():
            st.check_log(r, log, "run", 99, st.generated_build_info(dll.parent), False, run_dir / dll.name)
        st.print_stage(console, r)
        console.print(f"[dim]exit code {code}; report {report.get('report_file')}; log {log}[/]")

    # ---- unload -----------------------------------------------------------------------------------------------------
    @dev.command("unload")
    @click.option("--pid", type=int, help="Process id (default: every running MirrorsEdge.exe and test host).")
    def unload_cmd(pid: int | None) -> None:
        """Ask the menu to unload (like END), through its unload event (Local\\memenu-unload-<pid>)."""
        from .devtest import game_pids
        import csv, io, subprocess
        pids = [pid] if pid else game_pids()
        if not pid:
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq memenu_testhost.exe", "/FO", "CSV", "/NH"],
                                 capture_output=True, text=True, check=False).stdout
            pids += [int(row[1]) for row in csv.reader(io.StringIO(out)) if len(row) > 1 and row[1].isdigit()]
        if not pids:
            console.print("No Mirror's Edge or test host running.")
            return
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenEventW.restype = ctypes.c_void_p
        EVENT_MODIFY_STATE = 0x0002
        for p in pids:
            h = kernel32.OpenEventW(EVENT_MODIFY_STATE, False, f"Local\\memenu-unload-{p}")
            if not h:
                err = ctypes.get_last_error()
                why = ("the menu is not loaded there (or is an older build without the unload event)" if err == 2 else
                       "access denied: the game runs as administrator, run this from an admin terminal" if err == 5 else
                       f"error {err}")
                console.print(f"  pid {p}: [yellow]{why}[/]")
                continue
            kernel32.SetEvent(ctypes.c_void_p(h))
            kernel32.CloseHandle(ctypes.c_void_p(h))
            console.print(f"  pid {p}: [green]unload requested[/] (the log ends with 'Unloading' once it is done)")
