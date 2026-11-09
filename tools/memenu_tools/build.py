"""Configure, build and test through the CMake presets, with MSVC diagnostics shown as they come in."""

from __future__ import annotations

import re
import subprocess
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from rich.console import Console
from rich.markup import escape
from rich.progress import BarColumn, Progress, ProgressColumn, SpinnerColumn, Task, TextColumn, TimeElapsedColumn
from rich.table import Column, Table
from rich.text import Text

from .toolchain import LINUX, ROOT, Toolchain

# Linux builds (clang-cl + xwin) have presets and build folders of their own, so a checkout shared with Windows (WSL)
# never mixes the two.
CONFIGS = ({"debug": "linux-x86-debug", "release": "linux-x86-release"} if LINUX
           else {"debug": "x86-debug", "release": "x86-release"})
# The minimal build (`memenu minimal`): its own presets and build folders, so it never touches the full build's cache
MINIMAL = {"debug": "minimal-debug", "release": "minimal-release"}
PRESETS = CONFIGS | {"minimal-debug": "x86-minimal-debug", "minimal-release": "x86-minimal-release"}

# C:\path\file.cpp(12): error C2065: 'x': undeclared identifier      (also "(12,5)" and "fatal error")
# /path/file.cpp(12,5): error: use of undeclared identifier 'x'      (clang-cl: no code, sometimes a [-Wflag])
_COMPILER = re.compile(
    r"^(?P<file>(?:[A-Za-z]:)?[^:(]+)\((?P<line>\d+)(?:,\d+)?\)\s*:\s*(?P<kind>fatal error|error|warning)"
    r"(?:\s+(?P<code>[A-Z]+\d+))?\s*:\s*(?P<msg>.*)$"
)
# LINK : fatal error LNK1104: cannot open file 'x.lib'      /      foo.obj : error LNK2019: unresolved external ...
# lld-link: error: undefined symbol: ...
_LINKER = re.compile(r"^(?P<file>.+?)\s*:\s*(?P<kind>fatal error|error|warning)\s+(?P<code>LNK\d+)\s*:\s*(?P<msg>.*)$"
                     r"|^(?P<lld>lld-link)\s*:\s*(?P<lldkind>error|warning)\s*:\s*(?P<lldmsg>.*)$")
_PROGRESS = re.compile(r"^\[(\d+)/(\d+)\]\s+(.*)$")
# Ninja's step text, shortened for the progress bar: "Building CXX object src\CMakeFiles\...\menu.cpp.obj" -> "menu.cpp"
_STEP = re.compile(r"^(?P<verb>Building \w+ object|Linking \w+ (?:static library|shared library|executable)|"
                   r"Generating|Running|Copying|Re-running CMake)\s*(?P<what>.*)$")
_STEP_VERBS = {"Building": "compile", "Linking": "link", "Generating": "generate", "Running": "run",
               "Copying": "copy", "Re-running": "re-run CMake"}
# CMake configure: what CPM is fetching, and the configure/generate steps
_CONFIGURE_STEP = re.compile(r"^-- (?:CPM: (?:Adding package|Using local package) (?P<pkg>\S+)|"
                             r"(?P<phase>Configuring done|Generating done|Build files have been written).*)")


class _CountColumn(ProgressColumn):
    """ "12/340  3%", or nothing while the total isn't known yet (configuring, before Ninja's first line)."""

    def render(self, task: Task) -> Text:
        if task.total is None:
            return Text("")
        width = len(str(int(task.total)))
        return Text(f"{int(task.completed):>{width}}/{int(task.total)} {task.percentage:>3.0f}%", style="cyan")


def progress(console: Console, transient: bool = True) -> Progress:
    """The progress display every long step uses: spinner, title, bar, n/total, elapsed time and the current item.
    Tasks need a `step` field (the current item, shown dimmed at the end)."""
    return Progress(
        SpinnerColumn("dots", style="cyan"),
        TextColumn("{task.description}"),
        BarColumn(bar_width=28, pulse_style="cyan"),
        _CountColumn(),
        TimeElapsedColumn(),
        TextColumn("[dim]{task.fields[step]}[/]", table_column=Column(no_wrap=True, overflow="ellipsis", ratio=1)),
        console=console, transient=transient, expand=True,
    )


def _short_step(text: str) -> str:
    m = _STEP.match(text)
    if not m:
        return text
    verb = _STEP_VERBS.get(m["verb"].split()[0], m["verb"])
    what = m["what"].strip()
    name = Path(what.replace("\\", "/")).name if what else ""
    for suffix in (".obj", ".o"):
        name = name.removesuffix(suffix)
    return f"{verb} {name}".strip()


@dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int
    kind: str           # "error" | "warning" (fatal errors are errors)
    code: str
    message: str

    @property
    def location(self) -> str:
        path = Path(self.file)
        try:
            shown = str(path.resolve().relative_to(ROOT))
        except (ValueError, OSError):
            shown = self.file
        return f"{shown}({self.line})" if self.line else shown

    @property
    def ours(self) -> bool:
        """In this repository, not in a dependency."""
        try:
            Path(self.file).resolve().relative_to(ROOT)
        except (ValueError, OSError):
            return not Path(self.file).is_absolute()
        return "cmake-build-" not in self.file


def parse_diagnostic(line: str) -> Diagnostic | None:
    line = line.strip()
    if m := _COMPILER.match(line):
        kind = "warning" if m["kind"] == "warning" else "error"
        return Diagnostic(m["file"].strip(), int(m["line"]), kind, m["code"] or "clang", m["msg"].strip())
    if m := _LINKER.match(line):
        if m["lld"]:
            return Diagnostic("lld-link", 0, "warning" if m["lldkind"] == "warning" else "error", "lld",
                              m["lldmsg"].strip())
        kind = "warning" if m["kind"] == "warning" else "error"
        return Diagnostic(m["file"].strip(), 0, kind, m["code"], m["msg"].strip())
    return None


def build_dir(config: str) -> Path:
    return ROOT / f"cmake-build-{PRESETS[config]}"


# Optional parts of the project (CMake options, all OFF by default) and the targets they add.
OPTIONS = {
    "tests": "MEMENU_BUILD_TESTS",
    "installer": "MEMENU_BUILD_INSTALLER",
    "launcher": "MEMENU_BUILD_LAUNCHER",
    "testhost": "MEMENU_BUILD_TESTHOST",
    "benchmarks": "MEMENU_BUILD_BENCHMARKS",
}


def cache_value(config: str, name: str) -> str | None:
    """A variable's value from the build folder's CMakeCache.txt (None when unset or not configured)."""
    cache = build_dir(config) / "CMakeCache.txt"
    if not cache.exists():
        return None
    prefix = name + ":"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1].strip()
    return None


def option_enabled(config: str, name: str) -> bool:
    return (cache_value(config, name) or "").upper() in {"ON", "TRUE", "YES", "1", "Y"}


@dataclass
class RunResult:
    returncode: int
    diagnostics: list[Diagnostic]
    log: Path

    @property
    def errors(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if d.kind == "error"]

    @property
    def warnings(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if d.kind == "warning"]


def run_tool(console: Console, tc: Toolchain, args: list[str], title: str, log: Path,
             verbose: bool = False, show_warnings: str = "ours") -> RunResult:
    """Runs a build tool under a progress bar (Ninja's [n/total], CPM's packages while configuring), printing new
    diagnostics as they appear (and every line with `verbose`)."""
    log.parent.mkdir(parents=True, exist_ok=True)
    seen: set[Diagnostic] = set()
    ordered: list[Diagnostic] = []

    with log.open("w", encoding="utf-8", errors="replace") as out, progress(console) as bar:
        task = bar.add_task(title, total=None, step="starting")
        proc = subprocess.Popen(args, cwd=ROOT, env=tc.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
        assert proc.stdout is not None
        for raw in proc.stdout:
            out.write(raw)
            line = raw.rstrip()
            if verbose and line:
                console.print(escape(line), highlight=False)
            if m := _PROGRESS.match(line):
                bar.update(task, completed=int(m[1]), total=int(m[2]), step=escape(_short_step(m[3])))
            elif m := _CONFIGURE_STEP.match(line):
                bar.update(task, step=escape(f"CPM: {m['pkg']}" if m["pkg"] else m["phase"]))
            diag = parse_diagnostic(line)
            if not diag or diag in seen:
                continue
            seen.add(diag)
            ordered.append(diag)
            if verbose:
                continue
            if diag.kind == "error":
                console.print(f"[bold red]error[/] [red]{diag.code}[/] {escape(diag.location)}: {escape(diag.message)}")
            elif show_warnings == "all" or (show_warnings == "ours" and diag.ours):
                console.print(f"[yellow]warning {diag.code}[/] {escape(diag.location)}: {escape(diag.message)}")
        proc.wait()
    return RunResult(proc.returncode, ordered, log)


def configure(console: Console, tc: Toolchain, config: str, fresh: bool = False, verbose: bool = False,
              defines: tuple[str, ...] = ()) -> RunResult:
    args = [tc.cmake, "--preset", PRESETS[config]]
    if fresh:
        args.append("--fresh")
    args += [f"-D{d}" for d in defines]
    result = run_tool(console, tc, args, f"Configuring [bold]{config}[/]", build_dir(config) / "memenu-configure.log",
                      verbose=verbose)
    if result.returncode != 0 and not verbose:
        # CMake errors aren't MSVC diagnostics: show the end of the log.
        tail = result.log.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]
        console.print("\n".join(escape(t) for t in tail), highlight=False)
    return result


def is_configured(config: str) -> bool:
    return (build_dir(config) / "build.ninja").exists()


def build(console: Console, tc: Toolchain, config: str, targets: tuple[str, ...] = (), keep_going: bool = False,
          jobs: int | None = None, verbose: bool = False, show_warnings: str = "ours") -> RunResult:
    args = [tc.ninja, "-C", str(build_dir(config))]
    if keep_going:
        args += ["-k", "0"]
    if jobs:
        args += ["-j", str(jobs)]
    args += list(targets)
    label = f"Building [bold]{config}[/]" + (f" ({', '.join(targets)})" if targets else "")
    return run_tool(console, tc, args, label, build_dir(config) / "memenu-build.log", verbose, show_warnings)


def error_table(diagnostics: list[Diagnostic], include_warnings: bool = False) -> Table:
    """Errors (and warnings) grouped by file, most first."""
    by_file: dict[str, list[Diagnostic]] = defaultdict(list)
    for d in diagnostics:
        if d.kind == "error" or include_warnings:
            by_file[Path(d.location.split("(")[0]).as_posix()].append(d)

    table = Table(title="Diagnostics by file", show_lines=False, expand=True)
    table.add_column("File", style="cyan", no_wrap=True)
    table.add_column("Errors", justify="right", style="red")
    table.add_column("Warnings", justify="right", style="yellow")
    table.add_column("First problem", overflow="fold")
    for file, diags in sorted(by_file.items(), key=lambda kv: -len(kv[1])):
        errors = [d for d in diags if d.kind == "error"]
        first = (errors or diags)[0]
        table.add_row(file, str(len(errors)), str(len(diags) - len(errors)),
                      escape(f"{first.code} line {first.line}: {first.message}"))
    return table


# CTest's lines:  "  Start  12: Suite.Name"  /  " 12/115 Test  #12: Suite.Name .....   Passed    0.05 sec"
_CTEST_START = re.compile(r"^\s*Start\s+\d+:\s+(?P<name>\S+)")
_CTEST_RESULT = re.compile(r"^\s*(?P<n>\d+)/(?P<total>\d+)\s+Test\s+#\d+:\s+(?P<name>\S+)\s+\.*\s*"
                           r"(?P<status>.+?)\s+(?P<secs>[\d.]+)\s+sec\s*$")
_CTEST_SUMMARY = re.compile(r"^\d+% tests passed|^Total Test time|^The following tests|^No tests were found")


@dataclass
class TestResult:
    name: str
    status: str         # "passed" | "failed" | "skipped" | "not run"
    detail: str         # CTest's status text: "Passed", "Failed", "Exception: SegFault", ...
    seconds: float

    @property
    def suite(self) -> str:
        return self.name.split(".", 1)[0]


def _test_status(text: str) -> str:
    text = text.strip("* ").lower()
    for status in ("passed", "skipped", "not run"):
        if text.startswith(status):
            return status
    return "failed"


def run_ctest(console: Console, tc: Toolchain, config: str, name_filter: str | None = None,
              verbose: bool = False) -> int:
    """Runs CTest under a progress bar: failures (with their output) and skips are printed as they happen, then a
    table per test suite and the slowest tests. `verbose` prints CTest's every line as well (-V)."""
    args = [tc.ctest, "--test-dir", str(build_dir(config)), "--output-on-failure"]
    if name_filter:
        args += ["-R", name_filter]
    if verbose:
        args.append("-V")
    log = build_dir(config) / "memenu-test.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    results: list[TestResult] = []
    failing = False                 # Between a failed result and the next test: that test's output is coming in
    tail: list[str] = []

    with log.open("w", encoding="utf-8", errors="replace") as out, progress(console) as bar:
        task = bar.add_task("Testing", total=None, step="starting")
        proc = subprocess.Popen(args, cwd=ROOT, env=tc.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
        assert proc.stdout is not None
        for raw in proc.stdout:
            out.write(raw)
            line = raw.rstrip()
            tail = (tail + [line])[-30:]
            if verbose and line:
                console.print(escape(line), highlight=False)
            if m := _CTEST_START.match(line):
                failing = False
                bar.update(task, step=escape(m["name"]))
            elif m := _CTEST_RESULT.match(line):
                detail = m["status"].strip("* ")
                result = TestResult(m["name"], _test_status(detail), detail, float(m["secs"]))
                results.append(result)
                bar.update(task, completed=int(m["n"]), total=int(m["total"]))
                failing = result.status == "failed"
                if failing:
                    console.print(f"[bold red]✗ {escape(result.name)}[/] [red]{escape(detail)}[/] "
                                  f"[dim]{result.seconds:.2f}s[/]")
                elif result.status != "passed":
                    console.print(f"[yellow]○ {escape(result.name)}[/] [dim]{escape(detail)}[/]")
            elif _CTEST_SUMMARY.match(line):
                failing = False
            elif failing and not verbose and line.strip():
                console.print(f"    [dim]{escape(line)}[/]", highlight=False)
        proc.wait()

    if not results:
        if proc.returncode == 0:
            console.print("[yellow]No tests ran.[/]")
        else:
            console.print("\n".join(escape(t) for t in tail), highlight=False)
        console.print(f"[dim]Log: {log}[/]")
        return proc.returncode
    _print_test_summary(console, results)
    return proc.returncode


def _print_test_summary(console: Console, results: list[TestResult]) -> None:
    suites: dict[str, list[TestResult]] = defaultdict(list)
    for r in results:
        suites[r.suite].append(r)

    table = Table(expand=False, box=None, pad_edge=False, header_style="bold")
    table.add_column("Suite", style="cyan", no_wrap=True)
    table.add_column("Passed", justify="right", style="green")
    table.add_column("Failed", justify="right")
    table.add_column("Skipped", justify="right")
    table.add_column("Time", justify="right", style="dim")
    for suite, tests in sorted(suites.items()):
        failed = sum(t.status == "failed" for t in tests)
        skipped = sum(t.status in ("skipped", "not run") for t in tests)
        table.add_row(suite, str(sum(t.status == "passed" for t in tests)),
                      f"[bold red]{failed}[/]" if failed else "[dim]0[/]",
                      f"[yellow]{skipped}[/]" if skipped else "[dim]0[/]",
                      f"{sum(t.seconds for t in tests):.2f}s")
    console.print(table)

    slowest = sorted(results, key=lambda t: -t.seconds)[:3]
    console.print("[dim]Slowest: " + ", ".join(f"{escape(t.name)} {t.seconds:.2f}s" for t in slowest) + "[/]")
    failed = [t for t in results if t.status == "failed"]
    skipped = sum(t.status in ("skipped", "not run") for t in results)
    passed = len(results) - len(failed) - skipped
    counts = f"{passed} passed" + (f", {len(failed)} failed" if failed else "") + \
             (f", {skipped} skipped" if skipped else "") + \
             f" of {len(results)} in {sum(t.seconds for t in results):.1f}s"
    if failed:
        console.print(f"[bold red]Tests failed[/] ({counts}): " + ", ".join(escape(t.name) for t in failed))
    else:
        console.print(f"[bold green]All tests passed[/] ({counts})")
