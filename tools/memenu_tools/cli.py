"""`memenu`: one command for building, testing, installing and packaging the project.

    uv run memenu build                 debug build (configures first if needed)
    uv run memenu build -c release      release build
    uv run memenu errors                build everything, list every error grouped by file
    uv run memenu test                  build and run the unit tests
    uv run memenu install-game          build, then install into the game (UAC prompt)
    uv run memenu package               release installer -> dist/
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

import click
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from . import build as b
from .game import INSTALLER, LAUNCHER, detected_installs, project_version, run_elevated
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


def _build(ctx: click.Context, config: str, targets: tuple[str, ...] = (), keep_going: bool = False,
           jobs: int | None = None, verbose: bool = False, warnings: str = "ours") -> b.RunResult:
    tc = _toolchain(ctx)
    _ensure_configured(ctx, tc, config, verbose)
    start = time.perf_counter()
    result = b.build(console, tc, config, targets, keep_going, jobs, verbose, warnings)
    took = time.perf_counter() - start
    ours = [w for w in result.warnings if w.ours]
    if result.returncode == 0:
        console.print(f"[bold green]Build succeeded[/] ({config}, {took:.1f}s, {len(ours)} warning(s) in our code)")
    else:
        console.print(f"[bold red]Build failed[/] ({config}, {took:.1f}s, {len(result.errors)} error(s)). "
                      f"Full log: [cyan]{result.log}[/]")
    return result


def _artifact(config: str, name: str) -> Path:
    return b.build_dir(config) / name


# ---- commands ------------------------------------------------------------------------------------------------------
@click.group(context_settings={"help_option_names": ["-h", "--help"]})
@click.option("--refresh-env", is_flag=True, help="Capture the MSVC environment again (after a Visual Studio update).")
@click.pass_context
def cli(ctx: click.Context, refresh_env: bool) -> None:
    """Build, test, install and package Mirror's Edge Menu."""
    ctx.ensure_object(dict)["refresh"] = refresh_env


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
@verbose_option
@click.pass_context
def configure(ctx: click.Context, config: str, fresh: bool, defines: tuple[str, ...], verbose: bool) -> None:
    """Run CMake for a configuration."""
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
@verbose_option
@click.pass_context
def build_cmd(ctx: click.Context, config: str, targets: tuple[str, ...], clean_first: bool, keep_going: bool,
              jobs: int | None, warnings: str, verbose: bool) -> None:
    """Build (configures first when needed)."""
    if clean_first:
        _remove_build_dir(config)
    result = _build(ctx, config, targets, keep_going, jobs, verbose, warnings)
    if result.returncode == 0:
        for name in (INSTALLER, LAUNCHER, "medge_menu.dll"):
            if (p := _artifact(config, name)).exists():
                console.print(f"  [dim]{p.relative_to(ROOT)}[/]")
    ctx.exit(result.returncode and 1)


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
    """Build and run the unit tests (Catch2, through CTest)."""
    tc = _toolchain(ctx)
    if not no_build:
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
    configs = sorted(b.CONFIGS) if all_configs else [config]
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
    if not no_build and _build(ctx, config, ("memenu_installer",)).returncode != 0:
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
    if not no_build and _build(ctx, config, ("medge_launcher",)).returncode != 0:
        ctx.exit(1)
    launcher = _artifact(config, LAUNCHER)
    ctx.exit(subprocess.run([str(launcher), *ctx.args], cwd=launcher.parent).returncode)


@cli.command()
@click.option("--no-build", is_flag=True, help="Package what was built last.")
@click.pass_context
def package(ctx: click.Context, no_build: bool) -> None:
    """Build the release installer and copy it to dist/ (the one file players need)."""
    if not no_build and _build(ctx, "release", ("memenu_installer",)).returncode != 0:
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


def main() -> None:
    if sys.platform != "win32":
        Console(stderr=True).print("[red]This project builds on Windows only (32-bit MSVC).[/]")
        sys.exit(2)
    cli(obj={})
