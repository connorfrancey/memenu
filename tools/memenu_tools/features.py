"""Feature toggles: which modules go into medge_menu.dll (the options declared in cmake/Features.cmake).

CMake builds a feature when MEMENU_FEATURES, MEMENU_FEATURES_<CATEGORY> and MEMENU_FEATURE_<NAME> are all ON. The
CLI flattens that: it works out the on/off state of every feature, then writes all of them (with the master and
category switches ON), so `--disable all --enable god_mode` means exactly "only God Mode".
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from . import build as b
from .toolchain import ROOT

FEATURES_CMAKE = ROOT / "cmake" / "Features.cmake"
MASTER = "MEMENU_FEATURES"
ALL = "all"

# memenu_feature(<name> <category> [GAME] [OFF] STAGE <n> FILES ...)    memenu_stage(<n> "<what it adds>")
_DECL = re.compile(r"^\s*memenu_feature\(\s*(\w+)\s+(\w+)((?:\s+(?:GAME|OFF))*)\s+STAGE\s+(\d+)\s+FILES",
                   re.MULTILINE)
_STAGE = re.compile(r'^\s*memenu_stage\(\s*(\d+)\s+"([^"]*)"\s*\)', re.MULTILINE)


class UnknownFeature(ValueError):
    """A name that is not `all`, a category or a feature."""


@dataclass(frozen=True)
class Feature:
    name: str
    category: str
    game: bool          # Needs the game integration
    default: bool
    stage: int          # When the staged build (`memenu minimal --stage`) adds it back

    @property
    def var(self) -> str:
        return f"MEMENU_FEATURE_{self.name.upper()}"


def category_var(category: str) -> str:
    return f"MEMENU_FEATURES_{category.upper()}"


def _source() -> str:
    text = FEATURES_CMAKE.read_text(encoding="utf-8")
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())   # Drop comments (the examples in them)


def stages() -> list[tuple[int, str]]:
    """The staged build's stages, in order: (number, what it adds)."""
    found = [(int(m[1]), m[2]) for m in _STAGE.finditer(_source())]
    if not found:
        raise RuntimeError(f"No memenu_stage() declarations found in {FEATURES_CMAKE}")
    return found


def load() -> list[Feature]:
    found = []
    for m in _DECL.finditer(_source()):
        flags = m[3].split()
        found.append(Feature(m[1], m[2], "GAME" in flags, "OFF" not in flags, int(m[4])))
    if not found:
        raise RuntimeError(f"No memenu_feature() declarations found in {FEATURES_CMAKE}")
    return found


def categories(features: list[Feature]) -> list[str]:
    return list(dict.fromkeys(f.category for f in features))


def _norm(name: str) -> str:
    return name.strip().lower().replace("-", "_")


def resolve(features: list[Feature], name: str) -> list[Feature]:
    """`all`, a category or a feature name -> the features it means. UnknownFeature when it is none of them."""
    key = _norm(name)
    if key == ALL:
        return list(features)
    if key in categories(features):
        return [f for f in features if f.category == key]
    for f in features:
        if f.name == key:
            return [f]
    raise UnknownFeature(name)


def _specificity(features: list[Feature], name: str) -> int:
    key = _norm(name)
    return 0 if key == ALL else 1 if key in categories(features) else 2


def current_state(config: str, features: list[Feature]) -> dict[str, bool]:
    """What the build folder builds now (the defaults when it is not configured yet)."""
    if b.cache_value(config, MASTER) is None:
        return {f.name: f.default for f in features}
    master = b.option_enabled(config, MASTER)
    state = {}
    for f in features:
        own = b.option_enabled(config, f.var) if b.cache_value(config, f.var) is not None else f.default
        cat = b.cache_value(config, category_var(f.category))
        state[f.name] = master and (cat is None or b.option_enabled(config, category_var(f.category))) and own
    return state


def apply(features: list[Feature], state: dict[str, bool], only: tuple[str, ...], enable: tuple[str, ...],
          disable: tuple[str, ...], reset: bool = False) -> dict[str, bool]:
    """The new state. `reset` starts from the defaults in Features.cmake, `only` from everything off. Then the broadest names go first (all, then categories, then
    single features), so a narrower toggle always wins: `--disable all --enable god_mode`, `--enable debug --disable
    log_viewer`. Raises ValueError for an unknown name and for the same name enabled and disabled."""
    state = {f.name: f.default for f in features} if reset else dict(state)
    if only:
        state = {f.name: False for f in features}
        enable = only + enable
    clash = {_norm(n) for n in enable} & {_norm(n) for n in disable}
    if clash:
        raise ValueError(f"both enabled and disabled: {', '.join(sorted(clash))}")
    for name in [*enable, *disable]:
        resolve(features, name)   # Validate everything before changing anything
    toggles = [(n, True) for n in enable] + [(n, False) for n in disable]
    for name, on in sorted(toggles, key=lambda t: _specificity(features, t[0])):
        for f in resolve(features, name):
            state[f.name] = on
    return state


def defines(config: str, features: list[Feature], state: dict[str, bool]) -> tuple[str, ...]:
    """CMake -D values for `state`, only those that differ from the build folder's cache."""
    wanted = {MASTER: True}
    wanted |= {category_var(c): True for c in categories(features)}
    wanted |= {f.var: state[f.name] for f in features}
    out = []
    for var, on in wanted.items():
        cached = b.cache_value(config, var)
        if cached is None or b.option_enabled(config, var) != on:
            out.append(f"{var}={'ON' if on else 'OFF'}")
    return tuple(out)


def summary(features: list[Feature], state: dict[str, bool]) -> str:
    on = [f for f in features if state[f.name]]
    off = [f.name for f in features if not state[f.name]]
    game = any(f.game for f in on)
    text = f"{len(on)}/{len(features)} features, game integration {'on' if game else 'off'}"
    if not on:
        return text + " (menu GUI only)"
    if off and len(off) <= len(on):
        text += f"; off: {', '.join(off)}"
    elif off:
        text += f"; on: {', '.join(f.name for f in on)}"
    return text
