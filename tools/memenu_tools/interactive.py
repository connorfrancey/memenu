"""Interactive menu: what `memenu` (or `python memenu.py`) does when it is run without arguments.

Arrow keys and Enter to pick, Space to tick a checkbox, Ctrl+C (or Esc on a list) to go back. Every action runs the
same click command you could type yourself (shown as `= memenu ...` before it runs), so the menu never behaves
differently from the CLI.
"""

from __future__ import annotations

import shlex
from typing import Any

import click
import questionary
from prompt_toolkit.key_binding import KeyBindings, merge_key_bindings
from questionary import Choice, Separator
from rich.console import Console

from . import build as b
from . import features as feat
from . import devmode
from .game import project_version

PARTS = sorted(b.OPTIONS)   # installer, launcher, testhost, tests
BACK = "__back__"

STYLE = questionary.Style([
    ("qmark", "fg:#e5534b bold"),
    ("question", "bold"),
    ("pointer", "fg:#e5534b bold"),
    ("highlighted", "fg:#e5534b bold"),
    ("selected", "fg:#57ab5a"),
    ("separator", "fg:#768390"),
    ("instruction", "fg:#768390 italic"),
    ("disabled", "fg:#768390 italic"),
])

# Extra prompt_toolkit arguments for every question (the tests pass a scripted input and a dummy output here)
PROMPT_KWARGS: dict[str, Any] = {}


_ESCAPE = KeyBindings()


@_ESCAPE.add("escape")
def _escape(event) -> None:
    if not event.app.future.done():     # Already closing (Esc right after Enter / Ctrl+C): nothing to do
        event.app.exit(result=None)     # Esc = back, like Ctrl+C


def _ask(question: questionary.Question) -> Any:
    """The answer, or None when the user backed out (Ctrl+C / Esc)."""
    app = question.application
    app.key_bindings = merge_key_bindings([app.key_bindings, _ESCAPE]) if app.key_bindings else _ESCAPE
    return question.ask(kbi_msg="")


def select(message: str, choices: list, default: Any = None) -> Any:
    return _ask(questionary.select(message, choices=choices, default=default, style=STYLE, qmark="›",
                                   use_shortcuts=False, instruction="(arrows, Enter; Esc: back)", **PROMPT_KWARGS))


def checkbox(message: str, choices: list) -> list | None:
    return _ask(questionary.checkbox(message, choices=choices, style=STYLE, qmark="›",
                                     instruction="(Space: toggle, a: all, i: invert, Enter: done)", **PROMPT_KWARGS))


def confirm(message: str, default: bool = True) -> bool:
    return bool(_ask(questionary.confirm(message, default=default, style=STYLE, qmark="›", **PROMPT_KWARGS)))


class Menu:
    def __init__(self, cli: click.Group, console: Console) -> None:
        self.cli = cli
        self.console = console
        self.config = "debug"

    # ---- running commands ----------------------------------------------------------------------------------------
    def run(self, *args: str) -> int:
        """Runs `memenu <args>` in-process, like typing it. Returns its exit code."""
        self.console.print(f"[dim]= memenu {shlex.join(args)}[/]")
        try:
            code = self.cli.main(list(args), prog_name="memenu", standalone_mode=False, obj={})
        except click.exceptions.Abort:
            self.console.print("[yellow]Cancelled.[/]")
            return 1
        except click.ClickException as e:
            e.show()
            return e.exit_code
        except KeyboardInterrupt:
            self.console.print("[yellow]Interrupted.[/]")
            return 130
        return code or 0

    # ---- state shown in the menu ---------------------------------------------------------------------------------
    def _features_line(self) -> str:
        features = feat.load()
        return feat.summary(features, feat.current_state(self.config, features))

    def _parts_line(self) -> str:
        on = [p for p in PARTS if b.option_enabled(self.config, b.OPTIONS[p])]
        return ", ".join(on) if on else "none (only medge_menu.dll)"

    def _stage_line(self) -> str:
        cached = b.cache_value(b.MINIMAL[self.config], "MEMENU_MINIMAL_STAGE")
        last = feat.stages()[-1][0]
        return f"last built stage {cached}/{last}" if cached else f"not built yet (stages 0-{last})"

    def _optimizations_line(self) -> str:
        from .cli import optimizations_on
        return ("on (caches, scheduler intervals and budget)" if optimizations_on(self.config)
                else "off: everything does its work every time")

    def optimizations(self) -> None:
        from .cli import optimizations_on
        on = optimizations_on(self.config)
        choice = select(f"Optimizations ({self.config}): currently {'on' if on else 'off'}", [
            Choice("On: caches, cached object/name lookups, scheduler intervals and budget", value=True),
            Choice("Off (no-optimizations): same behavior, the work is done every time", value=False),
            Separator(),
            Choice("Back", value=BACK),
        ], default=on)
        if choice in (None, BACK):
            return
        if choice == on:
            self.console.print("Nothing changed.")
            return
        flag = "--optimizations" if choice else "--no-optimizations"
        if confirm("Build now?"):
            self.run("build", "-c", self.config, flag)
        else:
            self.run("configure", "-c", self.config, flag)

    def _other_config(self) -> str:
        return "release" if self.config == "debug" else "debug"

    # ---- main menu -----------------------------------------------------------------------------------------------
    def main(self) -> None:
        last = "build"
        while True:
            dev = devmode.enabled()
            self.console.rule(f"[bold]Mirror's Edge Menu {project_version()}[/] [dim]({self.config})[/]"
                              + (" [bold magenta]DEV[/]" if dev else ""))
            entries = [
                ("build", "Build", f"medge_menu.dll ({self.config})", self.build),
                ("features", "Features", self._features_line(), self.features),
                ("parts", "Optional parts", self._parts_line(), self.parts),
                ("optimizations", "Optimizations", self._optimizations_line(), self.optimizations),
                ("staged", "Staged build", self._stage_line(), self.staged),
                None,
                ("test", "Run tests", "GoogleTest unit tests", lambda: self.run("test", "-c", self.config)),
                ("install", "Install into the game", "builds the installer, asks for admin rights", self.install),
                ("devtest", "Dev test", "build, start the game with the menu, report crashes when you quit",
                 lambda: self.run("dev-test", "-c", self.config)),
                ("verbose", "Verbose build", "build stage by stage, test each one without the game",
                 lambda: self.run("verbose-build", "-c", self.config)),
                ("launch", "Launch", "inject without installing (asks for admin rights)", lambda: self.run("launch", "-c", self.config)),
                ("uninstall", "Uninstall from the game", "asks for admin rights", self.uninstall),
                ("package", "Package", "release installer -> dist/", lambda: self.run("package")),
                None,
                ("config", "Configuration", f"{self.config}; switch to {self._other_config()}", self.switch_config),
                ("errors", "Errors", "build everything, list errors by file",
                 lambda: self.run("errors", "-c", self.config)),
                ("info", "Info", "toolchain, build folders, game install", lambda: self.run("info")),
                ("copyinfo", "Copy diagnostic info", "logs, builds, game, crashes -> clipboard",
                 lambda: self.run("--copy-info")),
                ("clean", "Clean", f"delete the {self.config} build folder", self.clean),
            ]
            if dev:
                entries += [
                    None,
                    ("dev-env", "Dev: environment", "toolchain, build numbers and magic", lambda: self.run("dev", "env")),
                    ("dev-logs", "Dev: logs", "every menu log, current and archived", lambda: self.run("dev", "logs")),
                    ("dev-crashes", "Dev: crashes", "crash records resolved through the PDB",
                     lambda: self.run("dev", "crashes")),
                    ("dev-host", "Dev: test host", "load the last built DLL once, without the game",
                     lambda: self.run("dev", "host", "-c", self.config)),
                ]
            entries.append(("quit", "Quit", "", None))
            width = max(len(e[1]) for e in entries if e)
            choices = [Separator() if e is None else
                       Choice(title=[("", f"{e[1]:<{width}}  "), ("class:instruction", e[2])], value=e[0])
                       for e in entries]
            actions = {e[0]: e[3] for e in entries if e}
            choice = select("What do you want to do?", choices, default=last)
            if choice is None or choice == "quit":
                return
            last = choice
            self.console.print()
            actions[choice]()
            self.console.print()

    def switch_config(self) -> None:
        self.config = self._other_config()
        self.console.print(f"Now using [bold]{self.config}[/].")

    def build(self) -> None:
        self.run("build", "-c", self.config)

    def install(self) -> None:
        self.run("install-game", "-c", self.config)

    def uninstall(self) -> None:
        if confirm("Remove the menu from Mirror's Edge?"):
            settings = confirm("Also delete its config, themes and logs?", default=False)
            self.run("uninstall-game", "-c", self.config, *(["--remove-settings"] if settings else []))

    def clean(self) -> None:
        if confirm(f"Delete the {self.config} build folder? The next build starts from scratch.", default=False):
            self.run("clean", "-c", self.config, "--yes")

    # ---- features ------------------------------------------------------------------------------------------------
    def features(self) -> None:
        features = feat.load()
        applied = feat.current_state(self.config, features)
        how = select(f"Features ({self.config}): {feat.summary(features, applied)}", [
            Choice("Choose features...", value="pick"),
            Choice("Defaults (everything except TAS)", value="defaults"),
            Choice("Everything (TAS included)", value="all"),
            Choice("Nothing (menu GUI only, no game integration)", value="none"),
            Separator(),
            Choice("Back", value=BACK),
        ])
        if how in (None, BACK):
            return
        if how == "defaults":
            state = {f.name: f.default for f in features}
        elif how == "all":
            state = {f.name: True for f in features}
        elif how == "none":
            state = {f.name: False for f in features}
        else:
            choices: list = []
            for category in feat.categories(features):
                choices.append(Separator(f"── {category}"))
                for f in (x for x in features if x.category == category):
                    choices.append(Choice(title=f.name + ("  (needs game)" if f.game else ""), value=f.name,
                                          checked=applied[f.name]))
            picked = checkbox("Features to build", choices)
            if picked is None:
                return
            state = {f.name: f.name in picked for f in features}

        if state == applied:
            self.console.print("Nothing changed.")
            return
        self.console.print(f"-> {feat.summary(features, state)}")
        if confirm("Build now?"):
            self.run("build", "-c", self.config, *_toggle_args(features, state))
        else:   # Still remember the choice: configure only
            self.run("configure", "-c", self.config, *_toggle_args(features, state))

    # ---- optional parts ------------------------------------------------------------------------------------------
    def parts(self) -> None:
        current = {p: b.option_enabled(self.config, b.OPTIONS[p]) for p in PARTS}
        about = {"installer": "dinput8.dll loader + memenu_installer.exe",
                 "launcher": "medge_launcher.exe (injects without installing)",
                 "testhost": "memenu_testhost.exe (tests the DLL without the game)",
                 "tests": "GoogleTest unit tests"}
        picked = checkbox(f"Optional parts ({self.config})",
                          [Choice(title=f"{p:<10} {about.get(p, '')}", value=p, checked=current[p]) for p in PARTS])
        if picked is None:
            return
        changes = [f"{b.OPTIONS[p]}={'ON' if p in picked else 'OFF'}" for p in PARTS if (p in picked) != current[p]]
        if not changes:
            self.console.print("Nothing changed.")
            return
        if self.run("configure", "-c", self.config, *[a for c in changes for a in ("-D", c)]) == 0 \
                and confirm("Build now?"):
            self.build()

    # ---- staged build --------------------------------------------------------------------------------------------
    def staged(self) -> None:
        stages = feat.stages()
        features = feat.load()
        last = stages[-1][0]
        cached = b.cache_value(b.MINIMAL[self.config], "MEMENU_MINIMAL_STAGE")
        current = int(cached) if cached and cached.isdigit() else None
        default = 0 if current is None else min(current + 1, last)

        choices: list = []
        for n, what in stages:
            names = ", ".join(f.name for f in features if f.stage == n)
            tags = []
            if n == current:
                tags.append("last build")
            if n == default and n != current:
                tags.append("next")
            title = [("", f"{n:>2}  {what}")]
            if names:
                title.append(("class:instruction", f"  [{names}]"))
            if tags:
                title.append(("class:selected", f"  ← {', '.join(tags)}"))
            choices.append(Choice(title=title, value=n))
        choices += [Separator(), Choice("Back", value=BACK)]

        stage = select(f"Staged build ({self.config}): build up to which stage?", choices, default=default)
        if stage is None or stage == BACK:
            return
        self.run("minimal", "-c", self.config, "--stage", str(stage))


def _toggle_args(features: list[feat.Feature], state: dict[str, bool]) -> list[str]:
    """The shortest `memenu build` flags that give exactly `state`: from the defaults, from all on or from all off."""
    def flags(enable: list[str], disable: list[str]) -> list[str]:
        return [a for n in enable for a in ("--enable", n)] + [a for n in disable for a in ("--disable", n)]

    on = [f.name for f in features if state[f.name]]
    off = [f.name for f in features if not state[f.name]]
    candidates = [
        ["--default-features"] + flags([f.name for f in features if state[f.name] and not f.default],
                                       [f.name for f in features if not state[f.name] and f.default]),
        (["--enable", "all"] + flags([], off)) if off else ["--enable", "all"],
        (["--disable", "all"] + flags(on, [])) if on else ["--disable", "all"],
    ]
    return min(candidates, key=len)


def run(cli: click.Group, console: Console) -> None:
    try:
        Menu(cli, console).main()
    except (KeyboardInterrupt, EOFError):
        console.print()
