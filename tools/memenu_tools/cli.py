"""`memenu`: one command for building, testing, installing and packaging the project.

(or `python memenu.py ...` / `uv run memenu.py ...` from the repository root)

    uv run memenu                       interactive menu (everything below, picked from a list)
    uv run memenu build                 debug build of medge_menu.dll (configures first if needed)
    uv run memenu build -c release      release build
    uv run memenu build --with tests    also build an optional part (tests, installer, launcher)
    uv run memenu build --disable all   menu GUI only, no features (see `memenu features`)
    uv run memenu build --only levels   only the Levels features (also: --enable / --disable NAME)
    uv run memenu features              list the features and which ones the build folder builds
    uv run memenu dev-test              build, start the game with the menu injected, report crashes
    uv run memenu minimal               only the D3D9 hook + ImGui demo window (INSERT), nothing else
    uv run memenu errors                build everything, list every error grouped by file
    uv run memenu test                  build and run the unit tests
    uv run memenu install-game          build, then install into the game (UAC prompt)
    uv run memenu package               release installer -> dist/
    uv run memenu verbose-build         build stage by stage, test each one without the game, report failures
    uv run memenu --copy-info           copy diagnostic info (logs, builds, game, crashes) to the clipboard

Developer mode (MEMENU_DEV=1): full tracebacks, timings, and `memenu dev ...` tools (see devmode.py).
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import click
from rich.console import Console
from rich.markup import escape
from rich.panel import Panel
from rich.table import Table

from . import build as b
from . import features as feat
from .game import INSTALLER, LAUNCHER, detected_installs, is_elevated, project_version, run_elevated
from .toolchain import ROOT, Toolchain, ToolchainError, cmake_version, find_toolchain

console = Console(highlight=False)

config_option = click.option(
    "-c", "--config", type=click.Choice(sorted(b.CONFIGS)), default="debug", show_default=True,
    help="Build configuration.",
)
verbose_option = click.option("-v", "--verbose", is_flag=True, help="Print every line of the tool output.")


def _toolchain(ctx: click.Context) -> Toolchain:
    obj = ctx.ensure_object(dict)
    if "toolchain" not in obj:
        try:
            with console.status("Setting up the x86 MSVC environment"):
                obj["toolchain"] = find_toolchain(refresh=obj.get("refresh", False))
        except ToolchainError as e:
            console.print(f"[bold red]Toolchain problem:[/] {e}")
            ctx.exit(2)
    return obj["toolchain"]


def _ensure_configured(ctx: click.Context, tc: Toolchain, config: str, verbose: bool) -> None:
    if b.is_configured(config):
        return
    result = b.configure(console, tc, config, verbose=verbose)
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


def _enable_parts(ctx: click.Context, config: str, parts: tuple[str, ...] | list[str], verbose: bool = False) -> None:
    """Turn on optional parts (tests/installer/launcher) in a build folder, reconfiguring only if one is off.
    The setting stays in the CMake cache, so later plain builds keep building it."""
    missing = [b.OPTIONS[p] for p in parts if not b.option_enabled(config, b.OPTIONS[p])]
    if not missing:
        return
    console.print(f"Enabling {', '.join(missing)} in {b.build_dir(config).relative_to(ROOT)}")
    result = b.configure(console, _toolchain(ctx), config, verbose=verbose, defines=tuple(f"{m}=ON" for m in missing))
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


with_option = click.option(
    "--with", "parts", multiple=True, type=click.Choice(sorted(b.OPTIONS)),
    help="Also build an optional part (repeatable). Only medge_menu.dll is built by default.",
)


def feature_options(f):
    """--only / --enable / --disable NAME, where NAME is a feature, a category or `all`. Remembered by the build folder."""
    help_name = "a feature, a category or 'all' (see `memenu features`)"
    f = click.option("--disable", "disable", multiple=True, metavar="NAME",
                     help=f"Leave out {help_name} (repeatable).")(f)
    f = click.option("--enable", "enable", multiple=True, metavar="NAME",
                     help=f"Build {help_name} (repeatable).")(f)
    f = click.option("--default-features", "reset_features", is_flag=True,
                     help="Back to the defaults in cmake/Features.cmake (before any --only/--enable/--disable).")(f)
    f = click.option("--only", "only", multiple=True, metavar="NAME",
                     help=f"Build only these: {help_name} (repeatable).")(f)
    return f


def _feature_defines(ctx: click.Context, config: str, only: tuple[str, ...], enable: tuple[str, ...],
                     disable: tuple[str, ...], reset: bool = False) -> tuple[str, ...]:
    """The -D values the feature toggles need in this build folder (empty when there are no toggles or nothing changes)."""
    if not (only or enable or disable or reset):
        return ()
    features = feat.load()
    try:
        state = feat.apply(features, feat.current_state(config, features), only, enable, disable, reset)
    except feat.UnknownFeature as e:
        known = ", ".join([feat.ALL, *feat.categories(features)])
        raise click.BadParameter(f"unknown feature or category '{e}' (categories: {known}; "
                                 "`memenu features` lists the features)", ctx=ctx,
                                 param_hint="--only/--enable/--disable")
    except ValueError as e:
        raise click.BadParameter(str(e), ctx=ctx, param_hint="--only/--enable/--disable")
    defines = feat.defines(config, features, state)
    console.print(f"Features: {feat.summary(features, state)}" + ("" if defines else " [dim](unchanged)[/]"))
    return defines


OPTIMIZATIONS = "MEMENU_OPTIMIZATIONS"

optimizations_option = click.option(
    "--optimizations/--no-optimizations", "optimizations", default=None,
    help="Build the performance optimizations (caches, scheduler intervals and budget) or leave them out: without "
         "them everything works but does the work every time. Remembered by the build folder (default: on).")


def optimizations_on(config: str) -> bool:
    """Whether the build folder builds them (they default to on, also when it is not configured yet)."""
    return b.cache_value(config, OPTIMIZATIONS) is None or b.option_enabled(config, OPTIMIZATIONS)


def _optimization_defines(config: str, optimizations: bool | None) -> tuple[str, ...]:
    """The -D value --[no-]optimizations needs in this build folder (empty when not given or unchanged)."""
    if optimizations is None:
        return ()
    if b.cache_value(config, OPTIMIZATIONS) is not None and optimizations_on(config) == optimizations:
        return ()
    return (f"{OPTIMIZATIONS}={'ON' if optimizations else 'OFF'}",)


def _apply_features(ctx: click.Context, config: str, only: tuple[str, ...], enable: tuple[str, ...],
                    disable: tuple[str, ...], reset: bool, verbose: bool, extra: tuple[str, ...] = ()) -> None:
    defines = _feature_defines(ctx, config, only, enable, disable, reset) + extra
    if not defines:
        return
    result = b.configure(console, _toolchain(ctx), config, verbose=verbose, defines=defines)
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


def _build(ctx: click.Context, config: str, targets: tuple[str, ...] = (), keep_going: bool = False,
           jobs: int | None = None, verbose: bool = False, warnings: str = "ours") -> b.RunResult:
    tc = _toolchain(ctx)
    _ensure_configured(ctx, tc, config, verbose)
    start = time.perf_counter()
    result = b.build(console, tc, config, targets, keep_going, jobs, verbose, warnings)
    took = time.perf_counter() - start
    ours = [w for w in result.warnings if w.ours]
    if result.returncode == 0:
        note = "" if optimizations_on(config) else ", [yellow]no optimizations[/]"
        console.print(f"[bold green]Build succeeded[/] ({config}, {took:.1f}s, {len(ours)} warning(s) in our code{note})")
    else:
        console.print(f"[bold red]Build failed[/] ({config}, {took:.1f}s, {len(result.errors)} error(s)). "
                      f"Full log: [cyan]{result.log}[/]")
        _explain_locked_outputs(config, result)
    return result


def _explain_locked_outputs(config: str, result: b.RunResult) -> None:
    """LNK1168 (the linker can't overwrite its output): say which program has the file open."""
    from .locks import holders

    for d in result.errors:
        if d.code != "LNK1168":
            continue
        m = re.search(r"cannot open (.+?) for writing", d.message)
        if not m:
            continue
        path = Path(m[1].strip())
        if not path.is_absolute():
            path = b.build_dir(config) / path
        who = holders(path)
        if who:
            names = ", ".join(f"{name} (PID {pid})" for pid, name in who)
            console.print(f"[yellow]{path.name} is open in {names}[/]: the linker can't replace it while it is. "
                          "Close it there (an IDE: the file's tab, or a debug / attach session; the game: quit it, "
                          "or press END in it to unload the menu), then build again.")
        else:
            console.print(f"[yellow]Something had {path.name} open[/] (it may have closed it by now): build again.")


def built_dll(config: str) -> tuple[Path, Path | None]:
    """(the DLL in the build folder, its copy from MEMENU_COPY_TO_BUILD_DIR or None when there is no copy)."""
    dll = _artifact(config, "medge_menu.dll")
    copy_dir = b.cache_value(config, "MEMENU_BUILD_OUTPUT_DIR")
    copy = None
    if copy_dir and (b.cache_value(config, "MEMENU_COPY_TO_BUILD_DIR") is None
                     or b.option_enabled(config, "MEMENU_COPY_TO_BUILD_DIR")):
        copy = Path(copy_dir) / "medge_menu.dll"
    return dll, copy


def _report_dll(config: str) -> None:
    """Where the DLL that was just built is: the copy in build/ (the one to use) and the original in the build folder."""
    dll, copy = built_dll(config)
    if not dll.exists():
        console.print(f"[yellow]medge_menu.dll not found in {b.build_dir(config)}[/]")
        return
    size = f"{dll.stat().st_size / (1024 * 1024):.1f} MB"
    if copy and copy.exists() and copy.stat().st_size == dll.stat().st_size \
            and copy.stat().st_mtime >= dll.stat().st_mtime - 2:
        console.print(f"[bold]DLL:[/] [cyan]{copy.resolve()}[/] ({size})")
        console.print(f"  [dim]built in {dll.resolve()}[/]")
    else:
        console.print(f"[bold]DLL:[/] [cyan]{dll.resolve()}[/] ({size})")
        if copy:
            console.print(f"  [yellow]the copy in {copy.parent} is not this build[/]")


def _artifact(config: str, name: str) -> Path:
    return b.build_dir(config) / name


# ---- commands ------------------------------------------------------------------------------------------------------
@click.group(context_settings={"help_option_names": ["-h", "--help"]}, invoke_without_command=True)
@click.option("--refresh-env", is_flag=True, help="Capture the MSVC environment again (after a Visual Studio update).")
@click.option("--copy-info", is_flag=True,
              help="Copy everything needed to diagnose a problem (system, git, build folders, game install, crash "
                   "records, the menu's current and archived logs) to the clipboard, and exit.")
@click.pass_context
def cli(ctx: click.Context, refresh_env: bool, copy_info: bool) -> None:
    """Build, test, install and package Mirror's Edge Menu.

    Run it without a command for an interactive menu.
    """
    ctx.ensure_object(dict)["refresh"] = refresh_env
    if copy_info:
        _copy_info()
        ctx.exit(0)
    if ctx.invoked_subcommand is None:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            click.echo(ctx.get_help())      # Piped / CI: no menu to answer
            return
        try:
            from .interactive import run
        except ImportError as e:
            if e.name not in ("questionary", "prompt_toolkit"):
                raise
            console.print(f"[red]The interactive menu needs questionary ({e.name} is missing).[/] Run it with "
                          "`uv run memenu.py`, or: pip install questionary. Commands work without it (--help).")
            ctx.exit(2)
        run(cli, console)


def _copy_info() -> None:
    from .diaginfo import collect, copy_to_clipboard

    with console.status("Collecting diagnostic info"):
        report = collect()
    saved = ROOT / "build" / "diag-info.txt"
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_text(report, encoding="utf-8")
    try:
        copy_to_clipboard(report)
        console.print(f"[bold green]Diagnostic info copied to the clipboard[/] ({len(report.splitlines())} lines, "
                      f"{len(report) // 1024} KB). Also saved to {saved.relative_to(ROOT)}.")
    except OSError as e:
        console.print(f"[yellow]Could not use the clipboard ({e}).[/] The info is in {saved}.")
    console.print("[dim]It contains file paths (with your user name) and git info: check it before posting publicly.[/]")


@cli.command()
@click.pass_context
def info(ctx: click.Context) -> None:
    """Show the toolchain, build folders and the Mirror's Edge install that was found."""
    tc = _toolchain(ctx)
    table = Table(show_header=False, box=None, pad_edge=False)
    table.add_column(style="bold")
    table.add_column()
    table.add_row("Project version", project_version())
    table.add_row("Visual Studio", str(tc.vs_path))
    table.add_row("MSVC", tc.env.get("VCToolsVersion", "?"))
    table.add_row("CMake", f"{tc.cmake} ({'.'.join(map(str, cmake_version(tc.cmake) or ()))})")
    table.add_row("Ninja", tc.ninja)
    for config in sorted(b.CONFIGS):
        folder = b.build_dir(config)
        state = "configured" if b.is_configured(config) else "not configured"
        installer = _artifact(config, INSTALLER)
        if installer.exists():
            state += f", installer built {time.strftime('%Y-%m-%d %H:%M', time.localtime(installer.stat().st_mtime))}"
        table.add_row(f"Build ({config})", f"{folder.relative_to(ROOT)} [dim]({state})[/]")

    launcher = next((p for p in (_artifact(c, LAUNCHER) for c in ("debug", "release")) if p.exists()), None)
    found = detected_installs(launcher) if launcher else "(build the launcher to detect the game)"
    table.add_row("Game", found or "not found")
    console.print(Panel(table, title="Mirror's Edge Menu", expand=False))


@cli.command()
@click.option("--check-tag", metavar="TAG", help="Fail unless TAG (e.g. v0.1.0 or refs/tags/v0.1.0) is this version.")
def version(check_tag: str | None) -> None:
    """Print the project version (MEMENU_VERSION in CMakeLists.txt)."""
    current = project_version()
    if check_tag:
        tag = check_tag.removeprefix("refs/tags/").removeprefix("v")
        if tag != current:
            console.print(f"[bold red]Tag {check_tag} does not match MEMENU_VERSION {current}[/] "
                          "(update set(MEMENU_VERSION ...) in CMakeLists.txt, or tag v" + current + ")")
            sys.exit(1)
    click.echo(current)


@cli.command()
@config_option
@click.option("--fresh", is_flag=True, help="Configure from scratch (cmake --fresh).")
@click.option("-D", "defines", multiple=True, metavar="VAR=VALUE", help="CMake cache variable (repeatable).")
@with_option
@feature_options
@optimizations_option
@verbose_option
@click.pass_context
def configure(ctx: click.Context, config: str, fresh: bool, defines: tuple[str, ...], parts: tuple[str, ...],
              only: tuple[str, ...], enable: tuple[str, ...], disable: tuple[str, ...], reset_features: bool,
              optimizations: bool | None, verbose: bool) -> None:
    """Run CMake for a configuration."""
    if optimizations is not None:
        defines += (f"{OPTIMIZATIONS}={'ON' if optimizations else 'OFF'}",)
    defines = (_feature_defines(ctx, config, only, enable, disable, reset_features) + defines
               + tuple(f"{b.OPTIONS[p]}=ON" for p in parts))
    result = b.configure(console, _toolchain(ctx), config, fresh, verbose, defines)
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)
    console.print(f"[bold green]Configured[/] {b.build_dir(config).relative_to(ROOT)}")


@cli.command("build")
@config_option
@click.option("-t", "--target", "targets", multiple=True, help="Build only this target (repeatable).")
@click.option("--clean", "clean_first", is_flag=True, help="Delete the build folder first (full rebuild).")
@click.option("-k", "--keep-going", is_flag=True, help="Don't stop at the first failing file.")
@click.option("-j", "--jobs", type=int, help="Parallel jobs (default: Ninja's choice).")
@click.option("-w", "--warnings", type=click.Choice(["ours", "all", "none"]), default="ours", show_default=True,
              help="Which warnings to print.")
@with_option
@feature_options
@optimizations_option
@verbose_option
@click.pass_context
def build_cmd(ctx: click.Context, config: str, targets: tuple[str, ...], clean_first: bool, keep_going: bool,
              jobs: int | None, warnings: str, parts: tuple[str, ...], only: tuple[str, ...],
              enable: tuple[str, ...], disable: tuple[str, ...], reset_features: bool, optimizations: bool | None,
              verbose: bool) -> None:
    """Build (configures first when needed). By default only medge_menu.dll; see --with.

    
    Feature toggles are remembered by the build folder, so later plain builds keep them:
      memenu build --disable all                    menu GUI only, no in-game functionality
      memenu build --only levels --enable god_mode  the Levels tab and God Mode
      memenu build --enable debug --disable log_viewer
      memenu build --enable all                     everything, TAS included
      memenu build --default-features               back to the defaults (everything but TAS)
      memenu build --no-optimizations               no caches / scheduler intervals (--optimizations: back on)
    """
    if clean_first:
        _remove_build_dir(config)
    _apply_features(ctx, config, only, enable, disable, reset_features, verbose,
                    extra=_optimization_defines(config, optimizations))
    _enable_parts(ctx, config, parts, verbose)
    result = _build(ctx, config, targets, keep_going, jobs, verbose, warnings)
    if result.returncode == 0:
        _report_dll(config)
        for part, name in (("installer", INSTALLER), ("launcher", LAUNCHER)):   # Only parts this folder builds
            if b.option_enabled(config, b.OPTIONS[part]) and (p := _artifact(config, name)).exists():
                console.print(f"  {part}: [dim]{p}[/]")
    ctx.exit(result.returncode and 1)


@cli.command()
@config_option
@click.option("--clean", "clean_first", is_flag=True, help="Delete the minimal build folder first.")
@click.option("-s", "--stage", default=None, metavar="N|last",
              help="How far to go: 0 = bare hook ... last = every feature (remembered by the build folder; "
                   "--list shows the stages).")
@click.option("--next", "next_stage", is_flag=True, help="Go one stage further than the last build.")
@click.option("-l", "--list", "list_stages", is_flag=True, help="List the stages and what each adds, then exit.")
@click.option("-w", "--warnings", type=click.Choice(["ours", "all", "none"]), default="ours", show_default=True,
              help="Which warnings to print.")
@optimizations_option
@verbose_option
@click.pass_context
def minimal(ctx: click.Context, config: str, clean_first: bool, stage: str | None, next_stage: bool,
            list_stages: bool, warnings: str, optimizations: bool | None, verbose: bool) -> None:
    """Staged build: start from only what an injected overlay strictly needs, then add the trainer back one stage at a
    time, least likely to break the game first.

    \b
    Stage 0 is the D3D9 hook and ImGui's demo window (INSERT toggles it, END unloads), linking only MinHook, ImGui
    and d3d9. Each stage adds a layer or a group of features; the last one is the whole trainer, every feature on.
    The stages and each feature's stage are in cmake/Features.cmake.
      memenu minimal --list        what each stage adds
      memenu minimal --stage 4     build up to stage 4
      memenu minimal --next        one stage further than last time

    Uses its own build folders (cmake-build-x86-minimal-<config>) and copies the DLL to build/minimal/, so the full
    build is left alone. Inject it with: medge_launcher --dll build\\minimal\\medge_menu.dll
    """
    all_stages = feat.stages()
    features = feat.load()
    last = all_stages[-1][0]
    folder = b.MINIMAL[config]
    cached = b.cache_value(folder, "MEMENU_MINIMAL_STAGE")
    current = int(cached) if cached and cached.isdigit() else None

    if list_stages:
        console.print(_stage_table(all_stages, features, current))
        return
    if stage is not None and next_stage:
        raise click.UsageError("use --stage or --next, not both")
    if next_stage:
        if current is None:
            stage = "0"
        elif current >= last:
            console.print(f"Already at the last stage ({last}): every feature is on.")
            stage = str(last)
        else:
            stage = str(current + 1)
    if stage is not None:
        stage = str(last) if stage.lower() == "last" else stage
        if not stage.isdigit() or int(stage) > last:
            raise click.BadParameter(f"must be 0-{last} or 'last'", ctx=ctx, param_hint="--stage")

    if clean_first:
        _remove_build_dir(folder)
        cached = None
    defines = _optimization_defines(folder, optimizations)
    if stage is not None and cached != stage:
        defines += (f"MEMENU_MINIMAL_STAGE={stage}",)
    if defines:
        result = b.configure(console, _toolchain(ctx), folder, verbose=verbose, defines=defines)
        if result.returncode != 0:
            console.print(f"[bold red]Configure failed[/] (log: {result.log})")
            ctx.exit(1)
    shown = int(stage) if stage is not None else (current or 0)
    added = dict(all_stages).get(shown, "?")
    new = [f.name for f in features if f.stage == shown]
    console.print(f"[bold]Stage {shown}/{last}[/]: {added}" + (f" [dim]({', '.join(new)})[/]" if new else ""))
    built = [f for f in features if f.stage <= shown]
    if built:
        console.print(f"  {len(built)}/{len(features)} features" +
                      (", game integration on" if any(f.game for f in built) else ""))
    result = _build(ctx, folder, ("medge_menu",), verbose=verbose, warnings=warnings)
    if result.returncode == 0:
        _report_dll(folder)
        if shown < last:
            console.print(f"  [dim]Next: memenu minimal{' -c ' + config if config != 'debug' else ''} --next "
                          f"(stage {shown + 1}: {dict(all_stages).get(shown + 1, '?')})[/]")
    ctx.exit(result.returncode and 1)


@cli.command("verbose-build")
@config_option
@click.option("--from", "first", default="0", show_default=True, metavar="N", help="First stage to build and test.")
@click.option("--to", "last_stage", default="last", show_default=True, metavar="N|last", help="Last stage.")
@click.option("--seconds", default=2.0, show_default=True, help="How long the test host runs each phase.")
@click.option("-k", "--keep-going", is_flag=True, help="Test the remaining stages after a failure.")
@click.option("--no-unit-tests", is_flag=True, help="Skip the unit tests that run first.")
@click.option("--keep", is_flag=True, help="Keep every stage's DLL/PDB copies and test folders (dev mode: always).")
@verbose_option
@click.pass_context
def verbose_build(ctx: click.Context, config: str, first: str, last_stage: str, seconds: float, keep_going: bool,
                  no_unit_tests: bool, keep: bool, verbose: bool) -> None:
    """Build the menu in stages, adding features a few at a time, and test every stage thoroughly.

    \b
    First the unit tests, then for each stage of the staged build (memenu minimal --list):
      - build medge_menu.dll and memenu_testhost.exe for that stage
      - check the build: identity, 32-bit, no Visual C++ runtime DLLs, compiler warnings
      - load it into the test host (a D3D9 app, no game needed) twice: hook, draw, open the menu, device reset,
        unload; then check the menu's log for every milestone, errors, crashes, failed debug checks, the log archive
        and the saved config
    It stops at the first failing stage (--keep-going: goes on) and writes build/verbose-build/<time>/report.md with
    everything needed to find the cause (written for Claude Code; a prompt pointing to it is copied to the
    clipboard). Outside the game, the game integration can't start: those errors are expected and allowed.
    """
    from . import devmode
    from . import stagetest as st

    all_stages = feat.stages()
    last = all_stages[-1][0]
    lo = int(first) if first.isdigit() else -1
    hi = last if last_stage.lower() == "last" else (int(last_stage) if last_stage.isdigit() else -1)
    if not (0 <= lo <= hi <= last):
        raise click.BadParameter(f"stages are 0-{last}, --from must not be after --to", ctx=ctx)
    keep = keep or devmode.enabled()
    # One run at a time: two runs reconfigure and rebuild the same staged build folder under each other
    lock = st.SESSIONS / f".running-{config}"
    st.SESSIONS.mkdir(parents=True, exist_ok=True)
    if lock.exists():
        other = lock.read_text(encoding="utf-8").strip()
        if other.isdigit() and str(int(other)) in subprocess.run(
                ["tasklist", "/FI", f"PID eq {other}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout:
            console.print(f"[bold red]Another verbose-build ({config}) is running[/] (pid {other}): it uses the same "
                          "build folder. Wait for it to finish (or stop it), then run this again.")
            ctx.exit(2)
    lock.write_text(str(os.getpid()), encoding="utf-8")
    ctx.call_on_close(lambda: lock.unlink(missing_ok=True))
    tc = _toolchain(ctx)
    session = st.SESSIONS / datetime.now().strftime("%Y%m%d-%H%M%S")
    session.mkdir(parents=True, exist_ok=True)
    argv = "memenu " + " ".join(sys.argv[1:])
    started = time.time()

    unit: tuple[bool, str] | None = None
    if not no_unit_tests:
        console.rule("Unit tests")
        _enable_parts(ctx, config, ["tests"], verbose)
        r = _build(ctx, config, ("memenu_tests",), verbose=verbose)
        if r.returncode != 0:
            unit = (False, (r.log.read_text(encoding="utf-8", errors="replace") if r.log.exists() else "build failed"))
        else:
            p = subprocess.run([tc.ctest, "--test-dir", str(b.build_dir(config)), "--output-on-failure"], cwd=ROOT,
                               env=tc.env, capture_output=True, text=True, encoding="utf-8", errors="replace")
            unit = (p.returncode == 0, p.stdout + p.stderr)
        summary = next((line for line in unit[1].splitlines() if "tests passed" in line), "")
        console.print(f"  {'[green]passed[/]' if unit[0] else '[bold red]FAILED[/]'} [dim]{escape(summary)}[/]")

    results: list = []
    if unit is None or unit[0]:
        for stage, adds in all_stages:
            if not lo <= stage <= hi:
                continue
            new = [f.name for f in feat.load() if f.stage == stage]
            console.rule(f"Stage {stage}/{last}: {adds}")
            if new:
                console.print(f"  [dim]adds {', '.join(new)}[/]")
            r = st.test_stage(console, tc, config, stage, adds, session, seconds, verbose)
            results.append(r)
            st.print_stage(console, r)
            if not r.failed and not keep and r.folder and r.folder.exists():
                for big in list(r.folder.glob("*.dll")) + list(r.folder.glob("*.pdb")):
                    big.unlink(missing_ok=True)   # ~40 MB per stage in debug: only a failure's copies are kept
            if r.failed and not keep_going:
                break

    console.rule("Summary")
    if results:
        console.print(st.summary_table(results))
    failed = (unit is not None and not unit[0]) or any(r.failed for r in results)
    if failed:
        from .diaginfo import collect, copy_to_clipboard
        with console.status("Collecting debug info"):
            (session / "diag-info.txt").write_text(collect(), encoding="utf-8")
    report = st.write_report(session, config, results, unit, argv)
    console.print(f"Took {(time.time() - started) / 60:.1f} min. Report: [cyan]{report.relative_to(ROOT)}[/]")
    if failed:
        prompt = (f"`memenu verbose-build` failed. Read {report} (it has the failing stage, failed checks, build "
                  "diagnostics, the crash resolved to source lines, the menu's logs and the build setup), find the root "
                  "cause and fix it.")
        try:
            copy_to_clipboard(prompt)
            console.print("[bold red]Failed.[/] A prompt for Claude Code pointing to the report is on the clipboard.")
        except OSError:
            console.print(f"[bold red]Failed.[/] Give Claude Code this: {escape(prompt)}")
    else:
        console.print("[bold green]Every stage passed.[/]")
    old = sorted(p for p in st.SESSIONS.iterdir() if p.is_dir())
    for stale in old[:-10]:   # Keep the last 10 sessions
        shutil.rmtree(stale, ignore_errors=True)
    ctx.exit(1 if failed else 0)


def _stage_table(all_stages: list[tuple[int, str]], features: list[feat.Feature], current: int | None) -> Table:
    table = Table(title="Staged build (memenu minimal --stage N)", show_lines=True)
    table.add_column("Stage", justify="right", style="bold")
    table.add_column("Adds")
    table.add_column("Features", style="cyan")
    for n, what in all_stages:
        names = ", ".join(f.name for f in features if f.stage == n)
        mark = " [green]<- last build[/]" if n == current else ""
        table.add_row(str(n), what + mark, names)
    return table


@cli.command("features")
@config_option
def features_cmd(config: str) -> None:
    """List the features (cmake/Features.cmake) and which ones this build folder builds."""
    features = feat.load()
    state = feat.current_state(config, features)
    configured = b.cache_value(config, feat.MASTER) is not None
    table = Table(title=f"Features ({config}{'' if configured else ', not configured: defaults'})", box=None)
    table.add_column("Category", style="bold")
    table.add_column("Feature")
    table.add_column("Built")
    table.add_column("Needs game", style="dim")
    for category in feat.categories(features):
        for i, f in enumerate(x for x in features if x.category == category):
            table.add_row(category if i == 0 else "", f.name,
                          "[green]on[/]" if state[f.name] else "[red]off[/]", "yes" if f.game else "")
    console.print(table)
    console.print(f"{feat.summary(features, state)}\n[dim]Change with: memenu build -c {config} "
                  "--enable/--disable/--only NAME (a feature, a category or 'all')[/]")


@cli.command()
@config_option
@click.option("--warnings", "include_warnings", is_flag=True, help="List files with warnings too.")
@click.pass_context
def errors(ctx: click.Context, config: str, include_warnings: bool) -> None:
    """Build everything without stopping and list the errors grouped by file."""
    result = _build(ctx, config, keep_going=True, warnings="none")
    shown = result.diagnostics if include_warnings else result.errors
    if shown:
        console.print(b.error_table(shown, include_warnings))
    elif result.returncode == 0:
        console.print("[green]No errors.[/]")
    ctx.exit(result.returncode and 1)


@cli.command()
@config_option
@click.option("--no-build", is_flag=True, help="Run the tests as they were last built.")
@click.option("-R", "--filter", "name_filter", help="Only tests whose name matches this regex.")
@verbose_option
@click.pass_context
def test(ctx: click.Context, config: str, no_build: bool, name_filter: str | None, verbose: bool) -> None:
    """Build and run the unit tests (GoogleTest, through CTest)."""
    tc = _toolchain(ctx)
    if not no_build:
        _enable_parts(ctx, config, ["tests"], verbose)
        if _build(ctx, config, ("memenu_tests",), verbose=verbose).returncode != 0:
            ctx.exit(1)
    args = [tc.ctest, "--test-dir", str(b.build_dir(config)), "--output-on-failure"]
    if name_filter:
        args += ["-R", name_filter]
    if verbose:
        args.append("-V")
    console.rule("Tests")
    code = tc.run(args, cwd=ROOT).returncode
    console.print("[bold green]All tests passed[/]" if code == 0 else "[bold red]Tests failed[/]")
    ctx.exit(code and 1)


def _remove_build_dir(config: str) -> None:
    folder = b.build_dir(config)
    if folder.exists():
        with console.status(f"Removing {folder.relative_to(ROOT)}"):
            shutil.rmtree(folder)
        console.print(f"Removed {folder.relative_to(ROOT)}")


@cli.command()
@config_option
@click.option("--all", "all_configs", is_flag=True, help="Every configuration, and the cached MSVC environment.")
@click.option("-y", "--yes", is_flag=True, help="Don't ask.")
def clean(config: str, all_configs: bool, yes: bool) -> None:
    """Delete build folders (the dependency download cache is kept)."""
    configs = sorted(b.PRESETS) if all_configs else [config]
    folders = [b.build_dir(c) for c in configs if b.build_dir(c).exists()]
    if not folders and not all_configs:
        console.print("Nothing to clean.")
        return
    names = ", ".join(str(f.relative_to(ROOT)) for f in folders) or "the MSVC environment cache"
    if not yes and not click.confirm(f"Delete {names}?", default=True):
        return
    for c in configs:
        _remove_build_dir(c)
    if all_configs:
        (ROOT / ".cache" / "msvc-x86-env.json").unlink(missing_ok=True)


@cli.command("install-game")
@config_option
@click.option("--dir", "game_dir", type=click.Path(path_type=Path), help="Game folder (default: detected).")
@click.option("--no-build", is_flag=True, help="Install what was built last.")
@click.option("--gui", is_flag=True, help="Open the installer window instead of installing right away.")
@click.pass_context
def install_game(ctx: click.Context, config: str, game_dir: Path | None, no_build: bool, gui: bool) -> None:
    """Build, then install the menu into Mirror's Edge (asks for admin rights)."""
    _run_installer(ctx, config, ["--install"], game_dir, no_build, gui)


@cli.command("uninstall-game")
@config_option
@click.option("--dir", "game_dir", type=click.Path(path_type=Path), help="Game folder (default: detected).")
@click.option("--remove-settings", is_flag=True, help="Also delete the menu's config, themes and logs.")
@click.pass_context
def uninstall_game(ctx: click.Context, config: str, game_dir: Path | None, remove_settings: bool) -> None:
    """Remove the menu from Mirror's Edge (asks for admin rights)."""
    args = ["--uninstall"] + (["--remove-settings"] if remove_settings else [])
    _run_installer(ctx, config, args, game_dir, no_build=True, gui=False)


def _run_installer(ctx: click.Context, config: str, args: list[str], game_dir: Path | None, no_build: bool,
                   gui: bool) -> None:
    if not no_build:
        _enable_parts(ctx, config, ["installer"])
        if _build(ctx, config, ("memenu_installer",)).returncode != 0:
            ctx.exit(1)
    installer = _artifact(config, INSTALLER)
    if not installer.exists():
        console.print(f"[red]{installer.relative_to(ROOT)} has not been built yet.[/] Run: memenu build -c {config}")
        ctx.exit(1)
    if gui:
        args = []
    else:
        args = args + ["--quiet"] + ([str(game_dir.resolve())] if game_dir else [])
    console.print("Waiting for the installer (accept the administrator prompt)...")
    code = run_elevated(installer, args)
    if code == 0:
        console.print("[bold green]Done.[/]" if gui else
                      f"[bold green]Done:[/] {'installed into' if '--install' in args else 'removed from'} the game. "
                      "Start Mirror's Edge normally; INSERT opens the menu.")
    elif code == -1:
        console.print("[yellow]The administrator prompt was declined.[/]")
    else:
        console.print("[bold red]The installer failed[/] (is the game running?). Run with --gui to see the reason.")
    ctx.exit(0 if code == 0 else 1)


@cli.command(context_settings={"ignore_unknown_options": True, "allow_extra_args": True})
@config_option
@click.option("--no-build", is_flag=True, help="Use what was built last.")
@click.pass_context
def launch(ctx: click.Context, config: str, no_build: bool) -> None:
    """Inject without installing: run the launcher (extra arguments are passed on, e.g. --no-start)."""
    if not no_build:
        _enable_parts(ctx, config, ["launcher"])
        if _build(ctx, config, ("medge_launcher",)).returncode != 0:
            ctx.exit(1)
    launcher = _artifact(config, LAUNCHER)
    if not launcher.exists():
        console.print(f"[red]{launcher.relative_to(ROOT)} has not been built yet.[/] Run without --no-build.")
        ctx.exit(1)
    code = _run_launcher(launcher, list(ctx.args))
    if code == -1:
        console.print("[yellow]The administrator prompt was declined.[/]")
        ctx.exit(1)
    console.print("[bold green]Launcher finished.[/]" if code == 0 else f"[bold red]The launcher failed[/] (exit {code})")
    ctx.exit(0 if code == 0 else 1)


def _run_launcher(launcher: Path, args: list[str]) -> int:
    """Runs the launcher (it always runs as administrator). Returns its exit code, -1 when the prompt was declined."""
    if is_elevated():
        return subprocess.run([str(launcher), *args], cwd=launcher.parent).returncode
    console.print("Starting the launcher as administrator (accept the prompt; its output is in its own window, "
                  "press Enter there when it is done)...")
    return run_elevated(launcher, args)


@cli.command("dev-test", context_settings={"ignore_unknown_options": True, "allow_extra_args": True})
@config_option
@click.option("-s", "--stage", default=None, metavar="N|last",
              help="Test the staged build at this stage (memenu minimal) instead of the normal build.")
@click.option("--no-build", is_flag=True, help="Test what was built last.")
@click.option("--no-wait", is_flag=True, help="Don't wait for the game to close (no report).")
@click.pass_context
def dev_test(ctx: click.Context, config: str, stage: str | None, no_build: bool, no_wait: bool) -> None:
    """Build, start Mirror's Edge with the freshly built menu injected, and report how the run went once you quit.

    \b
    1. builds medge_menu.dll (or the staged build, --stage) and the launcher
    2. runs the launcher (as administrator): it starts the game if it isn't running, and injects the DLL
    3. waits until the game is closed, then shows the menu's log and, if the game crashed, where: Windows' crash
       record, the function and source line through the PDB, and the crash dump
    Extra arguments go to the launcher (e.g. --steam, --no-start). The build folder keeps its features and options:
    change them with `memenu build` (or `memenu minimal`) first.
    """
    from .devtest import crashes_since, dumps_since, game_pids, log_since, symbolize, wait_for_game_exit

    folder = config
    if stage is not None:
        folder = b.MINIMAL[config]
        last = feat.stages()[-1][0]
        stage = str(last) if stage.lower() == "last" else stage
        if not stage.isdigit() or int(stage) > last:
            raise click.BadParameter(f"must be 0-{last} or 'last'", ctx=ctx, param_hint="--stage")

    launcher = _artifact(config, LAUNCHER)
    if not no_build:
        _enable_parts(ctx, config, ["launcher"])
        if _build(ctx, config, ("medge_launcher",)).returncode != 0:
            ctx.exit(1)
        if stage is not None:
            if b.cache_value(folder, "MEMENU_MINIMAL_STAGE") != stage:
                result = b.configure(console, _toolchain(ctx), folder, defines=(f"MEMENU_MINIMAL_STAGE={stage}",))
                if result.returncode != 0:
                    console.print(f"[bold red]Configure failed[/] (log: {result.log})")
                    ctx.exit(1)
            console.print(f"Staged build, stage {stage}: {dict(feat.stages()).get(int(stage), '?')}")
            if _build(ctx, folder, ("medge_menu",)).returncode != 0:
                ctx.exit(1)

    # The DLL straight from its build folder: its PDB is next to it, and so is the logs/memenu.log it writes.
    dll = _artifact(folder, "medge_menu.dll")
    for path, what in ((launcher, "launcher"), (dll, "DLL")):
        if not path.exists():
            console.print(f"[red]No {what} at {path}.[/] Run without --no-build.")
            ctx.exit(1)
    console.print(f"[bold]Injecting:[/] [cyan]{dll}[/]")
    if game_pids():
        console.print("[yellow]Mirror's Edge is already running:[/] the menu goes into that game. If a medge_menu.dll "
                      "is already loaded there, the launcher does nothing (close the game for a clean test).")

    start = datetime.now()
    code = _run_launcher(launcher, ["--dll", str(dll), *ctx.args])
    if code == -1:
        console.print("[yellow]The administrator prompt was declined.[/]")
        ctx.exit(1)
    if code != 0:
        console.print(f"[bold red]The launcher failed[/] (exit {code}): see its window for the reason.")
    if no_wait:
        ctx.exit(0 if code == 0 else 1)

    if game_pids():
        console.print("Game running. [bold]Quit Mirror's Edge to see the report[/] (Ctrl+C: stop waiting).")
        try:
            with console.status("Waiting for Mirror's Edge to close"):
                wait_for_game_exit()
        except KeyboardInterrupt:
            console.print("Stopped waiting: no report.")
            ctx.exit(0)
        time.sleep(3)   # Windows writes the crash record a moment after the process is gone

    # ---- report -----------------------------------------------------------------------------------------------------
    console.rule("Dev test report")
    lines = log_since(dll, start)
    if lines is None:
        console.print(f"[bold red]The menu never started:[/] no logs/memenu.log was written next to the DLL "
                      f"({dll.parent}). If the launcher said 'already loaded', another medge_menu.dll was in the game "
                      "(the dinput8 loader's installed copy, or an earlier injection).")
    else:
        console.print(f"[bold]memenu.log[/] (last {min(len(lines), 20)} of {len(lines)} lines):")
        for line in lines[-20:]:
            style = "red" if "[error]" in line or "[critical]" in line else "yellow" if "[warning]" in line else "dim"
            console.print(f"  [{style}]{escape(line)}[/]")
        if not any("Unloading" in line for line in lines):
            console.print("  [yellow](the log ends without 'Unloading': the menu didn't unload, the game was closed "
                          "or crashed with it loaded)[/]")

    crashes = crashes_since(start)
    if not crashes:
        console.print("[bold green]No crash recorded.[/]")
    for c in crashes:
        console.print(f"[bold red]Crash[/] at {c.time}: [bold]{c.module}+{c.offset:#x}[/], exception {c.code}"
                      + (f" ({c.description})" if c.description else ""))
        if c.module.lower() == "medge_menu.dll":
            where = symbolize(Path(c.module_path) if c.module_path else dll, c.offset)
            console.print(f"  at {escape(where)}" if where else "  [dim](no symbols for that build)[/]")
            if where and "\\rtc\\" in where.lower():
                console.print("  [dim]a /RTC run-time check fired there: the code that tripped it is further up the "
                              "stack, in the crash dump[/]")
    for dump in dumps_since(start):
        console.print(f"  dump: [cyan]{dump}[/]")
    ctx.exit(1 if crashes or code != 0 else 0)


@cli.command("game-test")
@config_option
@click.option("--no-build", is_flag=True, help="Test what was built last.")
@click.option("-B", "--build-dir", type=click.Path(path_type=Path, file_okay=False),
              help="Take medge_menu.dll and loader/dinput8.dll from this build folder (implies --no-build).")
@click.option("--dir", "game_dir", type=click.Path(path_type=Path, file_okay=False), help="Game folder (default: detected).")
@click.option("--map", "map_name", default="edge_p", show_default=True,
              help="Level to load before testing the modules ('none': stay in the main menu).")
@click.option("--checkpoint", default=2, show_default=True,
              help="Checkpoint of that level to start at (0: the level start; the Prologue's first two are its intro).")
@click.option("--ticks", default=10, show_default=True, help="Engine ticks (and frames) each module stays on.")
@click.option("--only", default="", metavar="A,B", help="Test only these modules (by name).")
@click.option("--skip", default="", metavar="A,B", help="Don't test these modules.")
@click.option("--late", "late_s", default=0.0, metavar="SECONDS",
              help="Load the menu this much later than usual (after the game went fullscreen, like the launcher).")
@click.option("--timeout", default=900.0, show_default=True, help="Kill the game after this many seconds.")
@click.option("--keep-open", is_flag=True, help="Don't close the game when the test is done.")
@click.pass_context
def game_test(ctx: click.Context, config: str, no_build: bool, build_dir: Path | None, game_dir: Path | None,
              map_name: str, checkpoint: int, ticks: int, only: str, skip: str, late_s: float, timeout: float,
              keep_open: bool) -> None:
    """Run the in-game self-test in Mirror's Edge and report every step (see tools/memenu_tools/gametest.py).

    \b
    Installs the build into the game folder (loader + menu, like the installer, no admin needed when the folder is
    writable), starts the game, and the menu tests itself: hooks and drawing, the engine tick, loading a level, then
    every module switched on and off (all its key-bindable controls pressed, menu open and closed), all modules on at
    once, and a clean unload. Then it closes the game. Exit code 0 only when every step passed and nothing crashed.
    """
    from . import devtest
    from . import gametest as gt

    if layers := gt.compat_layers():
        console.print(f"[yellow]MirrorsEdge.exe has compatibility settings ({escape(layers)})[/]: \"Run as "
                      "administrator\" and the Windows XP/98 modes make the game start elevated, which this tool "
                      "can't start or watch. Clear them in the exe's Properties > Compatibility, or run this from an "
                      "administrator terminal.")
    game_dir = game_dir or gt.find_game_dir()
    if not game_dir or not (game_dir / devtest.GAME_EXE).exists():
        console.print("[red]Mirror's Edge not found.[/] Pass --dir <...\\Binaries>.")
        ctx.exit(1)

    if build_dir:
        folder = build_dir if build_dir.is_absolute() else ROOT / build_dir
    else:
        folder = b.build_dir(config)
        if not no_build:
            _enable_parts(ctx, config, ["installer"])   # The loader comes with the installer option
            if _build(ctx, config, ("medge_menu", "memenu_loader")).returncode != 0:
                ctx.exit(1)
    dll, loader = folder / "medge_menu.dll", folder / "loader" / "dinput8.dll"
    for f in (dll, loader):
        if not f.exists():
            console.print(f"[red]{f} is missing[/]: build with the installer part (memenu build --with installer).")
            ctx.exit(1)
    if devtest.game_pids():
        console.print("[red]Mirror's Edge is running.[/] Close it first.")
        ctx.exit(1)
    if (game_dir / "memenu" / "disabled").exists():
        console.print("[yellow]memenu\\disabled exists: the loader would skip the menu. Removed it.[/]")
        (game_dir / "memenu" / "disabled").unlink()

    gt.install(game_dir, dll, loader)
    gt.write_options(game_dir, {"map": map_name, "checkpoint": str(checkpoint), "ticks": str(ticks), "only": only, "skip": skip,
                                "quit": "0" if keep_open else "1"}, int(late_s * 1000))
    console.print(f"Testing [cyan]{dll}[/] in [cyan]{game_dir}[/]" + (f", loaded {late_s:.0f} s late" if late_s else ""))

    def progress(res: gt.Result) -> None:
        if res.current:
            console.print(f"  [dim]{res.current}...[/]")

    try:
        res = gt.run(game_dir, timeout, progress)
    except gt.ElevationRequired:
        console.print("[red]Windows wants administrator rights to start the game[/] (compatibility settings on "
                      "MirrorsEdge.exe, see above): run this from an administrator terminal, or clear them.")
        ctx.exit(1)
    finally:
        gt.cleanup(game_dir)

    table = Table(show_header=True, header_style="bold")
    table.add_column("Step")
    table.add_column("Result")
    table.add_column("Time", justify="right")
    table.add_column("Details")
    for s in res.steps:
        details = "\n".join(s.get("problems", []) + [f"[dim]{n}[/]" for n in s.get("notes", [])][:12])
        table.add_row(escape(s["name"]), "[green]ok[/]" if s["ok"] else "[bold red]FAILED[/]", f"{s['seconds']:.1f}s",
                      details)
    console.print(table)
    if res.current:
        console.print(f"[bold red]Stopped in the middle of: {escape(res.current)}[/]")
    console.print(f"{res.exit_reason}, {res.seconds:.0f} s")
    for c in res.crashes:
        where = devtest.symbolize(game_dir / "memenu" / "medge_menu.dll", c.offset) if "medge_menu" in c.module.lower() else None
        console.print(f"[bold red]Crash[/] {c.time} {c.code} {c.description} in {c.module}+{c.offset:#x}"
                      + (f"\n  at {escape(where)}" if where else ""))
    if res.log_errors:
        console.print(f"[yellow]{len(res.log_errors)} error line(s) in the menu's log:[/]")
        for line in res.log_errors[:20]:
            console.print(f"  {escape(line)}")
    ok = res.finished and res.passed and not res.crashes
    console.print("[bold green]Self-test passed.[/]" if ok else "[bold red]Self-test failed.[/]")
    ctx.exit(0 if ok else 1)


@cli.command()
@click.option("--no-build", is_flag=True, help="Package what was built last.")
@click.pass_context
def package(ctx: click.Context, no_build: bool) -> None:
    """Build the release installer and copy it to dist/ (the one file players need)."""
    if not no_build:
        _enable_parts(ctx, "release", ["installer"])
        if _build(ctx, "release", ("memenu_installer",)).returncode != 0:
            ctx.exit(1)
    src = _artifact("release", INSTALLER)
    if not src.exists():
        console.print("[red]No release installer yet.[/] Run without --no-build.")
        ctx.exit(1)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    # Versioned name for people browsing releases, fixed name for install.ps1 (releases/latest/download/<name>).
    versioned = dist / f"memenu_installer-{project_version()}.exe"
    fixed = dist / INSTALLER
    shutil.copy2(src, versioned)
    shutil.copy2(src, fixed)
    digest = hashlib.sha256(versioned.read_bytes()).hexdigest()
    (dist / "SHA256SUMS.txt").write_text(f"{digest}  {versioned.name}\n{digest}  {fixed.name}\n", encoding="ascii")
    console.print(Panel.fit(f"[bold]{versioned.relative_to(ROOT)}[/] (and {fixed.relative_to(ROOT)})\n"
                            f"{versioned.stat().st_size / 1e6:.1f} MB\nsha256 {digest}",
                            title="Packaged", border_style="green"))


from . import devmode as _devmode   # noqa: E402 - registers `memenu dev ...` on the group above

_devmode.register(cli, console)


def main() -> None:
    if sys.platform != "win32":
        Console(stderr=True).print("[red]This project builds on Windows only (32-bit MSVC).[/]")
        sys.exit(2)
    if _devmode.enabled():
        _devmode.setup(console)
    cli(obj={})
