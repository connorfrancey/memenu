"""Configure, build and test through the CMake presets, with MSVC diagnostics shown as they come in."""

from __future__ import annotations

import re
import subprocess
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from rich.console import Console
from rich.markup import escape
from rich.table import Table

from .toolchain import ROOT, Toolchain

CONFIGS = {"debug": "x86-debug", "release": "x86-release"}

# C:\path\file.cpp(12): error C2065: 'x': undeclared identifier      (also "(12,5)" and "fatal error")
_COMPILER = re.compile(
    r"^(?P<file>(?:[A-Za-z]:)?[^:(]+)\((?P<line>\d+)(?:,\d+)?\)\s*:\s*(?P<kind>fatal error|error|warning)\s+"
    r"(?P<code>[A-Z]+\d+)\s*:\s*(?P<msg>.*)$"
)
# LINK : fatal error LNK1104: cannot open file 'x.lib'      /      foo.obj : error LNK2019: unresolved external ...
_LINKER = re.compile(r"^(?P<file>.+?)\s*:\s*(?P<kind>fatal error|error|warning)\s+(?P<code>LNK\d+)\s*:\s*(?P<msg>.*)$")
_PROGRESS = re.compile(r"^\[(\d+)/(\d+)\]\s+(.*)$")


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
        return Diagnostic(m["file"].strip(), int(m["line"]), kind, m["code"], m["msg"].strip())
    if m := _LINKER.match(line):
        kind = "warning" if m["kind"] == "warning" else "error"
        return Diagnostic(m["file"].strip(), 0, kind, m["code"], m["msg"].strip())
    return None


def build_dir(config: str) -> Path:
    return ROOT / f"cmake-build-{CONFIGS[config]}"


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
    """Runs a build tool, printing new diagnostics as they appear (and every line with `verbose`)."""
    log.parent.mkdir(parents=True, exist_ok=True)
    seen: set[Diagnostic] = set()
    ordered: list[Diagnostic] = []

    with log.open("w", encoding="utf-8", errors="replace") as out, console.status(title) as status:
        proc = subprocess.Popen(args, cwd=ROOT, env=tc.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
        assert proc.stdout is not None
        for raw in proc.stdout:
            out.write(raw)
            line = raw.rstrip()
            if verbose and line:
                console.print(escape(line), highlight=False)
            if m := _PROGRESS.match(line):
                status.update(f"{title} [dim][{m[1]}/{m[2]}][/] {escape(m[3][:90])}")
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
    args = [tc.cmake, "--preset", CONFIGS[config]]
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
