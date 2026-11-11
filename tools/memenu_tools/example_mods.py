"""The example Mirage mods (examples/mods): build them the way a mod author would, outside the menu's build.

The native one (hello) is its own CMake project, built against the mod API headers only (api/mirage). With clang-cl
available it is built with clang-cl: a different compiler from the menu's MSVC, which is what the ABI promises to
support. The Lua one (hello-lua) needs no build. `memenu game-test` installs both into the game's memenu/selftest-mods
for the self-test step "Mirage mods" and removes them after.
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from .toolchain import Toolchain

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples" / "mods"
OUT = ROOT / "build" / "example-mods"


def clang_cl() -> str | None:
    return shutil.which("clang-cl")


def build_native(tc: Toolchain, name: str = "hello", compiler: str | None = None) -> tuple[Path | None, str]:
    """Builds examples/mods/<name> in its own build folder. Returns (the built mod's folder, a log tail)."""
    compiler = compiler if compiler is not None else (clang_cl() or "")
    tag = "clang" if compiler and "clang" in Path(compiler).name.lower() else "msvc"
    build = OUT / f"{name}-{tag}"
    configure = [tc.cmake, "-S", str(EXAMPLES / name), "-B", str(build), "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={tc.ninja}", "-DCMAKE_BUILD_TYPE=Release"]
    if tag == "clang":
        configure += [f"-DCMAKE_CXX_COMPILER={compiler}", "-DCMAKE_CXX_FLAGS=-m32"]
    else:
        configure += ["-DCMAKE_CXX_COMPILER=cl.exe"]
    log = ""
    for cmd in (configure, [tc.cmake, "--build", str(build)]):
        r = subprocess.run(cmd, env=tc.env, capture_output=True, text=True)
        log += r.stdout + r.stderr
        if r.returncode != 0:
            return None, log[-3000:]
    return build / "mod" / name, f"{name} built with {tag}"


def install_for_test(game_dir: Path, folders: list[Path]) -> Path:
    """Copies each mod folder into <game>/memenu/selftest-mods/<folder name> (emptied first)."""
    target = game_dir / "memenu" / "selftest-mods"
    if target.exists():
        shutil.rmtree(target)
    for f in folders:
        shutil.copytree(f, target / f.name, ignore=shutil.ignore_patterns("config.toml"))
    return target


def remove_test_mods(game_dir: Path) -> None:
    target = game_dir / "memenu" / "selftest-mods"
    if target.exists():
        shutil.rmtree(target, ignore_errors=True)
