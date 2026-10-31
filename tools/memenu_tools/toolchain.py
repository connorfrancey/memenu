"""Finding the 32-bit MSVC toolchain, CMake and Ninja.

The game is a 32-bit process, so everything is built with the x86 MSVC tools. Instead of requiring a
"Developer Command Prompt", the environment that ``vcvarsall.bat x86`` sets up is captured once and
cached in ``.cache/`` (refreshed when Visual Studio changes).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE_DIR = ROOT / ".cache"
VS_INSTALLER = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer"
VSWHERE = VS_INSTALLER / "vswhere.exe"
MIN_CMAKE = (4, 2)


class ToolchainError(RuntimeError):
    """Something needed for the build is missing."""


@dataclass(frozen=True)
class Toolchain:
    vs_path: Path
    env: dict[str, str]
    cmake: str
    ninja: str

    @property
    def ctest(self) -> str:
        exe = Path(self.cmake).with_name("ctest.exe")
        return str(exe) if exe.exists() else "ctest"

    def run(self, args: list[str], **kwargs) -> subprocess.CompletedProcess:
        return subprocess.run(args, env=self.env, **kwargs)


def find_visual_studio() -> Path:
    if not VSWHERE.exists():
        raise ToolchainError(
            "Visual Studio was not found (no vswhere.exe). Install Visual Studio or the Build Tools with "
            '"Desktop development with C++".'
        )
    result = subprocess.run(
        [str(VSWHERE), "-latest", "-prerelease", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=False,
    )
    paths = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not paths:
        raise ToolchainError('No Visual Studio install has the C++ x86/x64 tools ("Desktop development with C++").')
    return Path(paths[-1])


def _capture_vcvars(vcvars: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["PATH"] = f"{VS_INSTALLER};{env.get('PATH', '')}"     # vcvarsall runs vswhere itself
    # One string, not a list: cmd.exe needs these exact quotes, which list2cmdline would escape.
    command = f'cmd.exe /d /s /c ""{vcvars}" x86 >nul && set"'
    result = subprocess.run(command, capture_output=True, text=True, env=env, check=False)
    if result.returncode != 0:
        raise ToolchainError(f"vcvarsall.bat x86 failed:\n{result.stdout}{result.stderr}")
    captured = {}
    for line in result.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            captured[key] = value
    if captured.get("VSCMD_ARG_TGT_ARCH", "").lower() != "x86":
        raise ToolchainError("vcvarsall.bat did not set up an x86 environment.")
    return captured


def msvc_env(vs_path: Path, refresh: bool = False) -> dict[str, str]:
    if os.environ.get("VSCMD_ARG_TGT_ARCH", "").lower() == "x86":
        return dict(os.environ)     # Already in an x86 developer prompt

    vcvars = vs_path / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    if not vcvars.exists():
        raise ToolchainError(f"{vcvars} is missing: repair the Visual Studio C++ tools.")

    cache = CACHE_DIR / "msvc-x86-env.json"
    key = f"{vcvars}|{vcvars.stat().st_mtime_ns}"
    if not refresh and cache.exists():
        try:
            data = json.loads(cache.read_text(encoding="utf-8"))
            if data.get("key") == key:
                return data["env"]
        except (OSError, ValueError, KeyError):
            pass

    env = _capture_vcvars(vcvars)
    CACHE_DIR.mkdir(exist_ok=True)
    cache.write_text(json.dumps({"key": key, "env": env}), encoding="utf-8")
    return env


def cmake_version(cmake: str) -> tuple[int, ...] | None:
    try:
        out = subprocess.run([cmake, "--version"], capture_output=True, text=True, check=False).stdout
    except OSError:
        return None
    match = re.search(r"cmake version (\d+)\.(\d+)(?:\.(\d+))?", out)
    return tuple(int(g) for g in match.groups() if g is not None) if match else None


def find_toolchain(refresh: bool = False) -> Toolchain:
    vs = find_visual_studio()
    env = msvc_env(vs, refresh)

    # vcvars puts Visual Studio's bundled CMake first, which may be older than this project needs: prefer the CMake
    # that was on PATH before, if it is new enough.
    candidates = [shutil.which("cmake"), shutil.which("cmake", path=env.get("PATH") or env.get("Path"))]
    cmake = next((c for c in candidates if c and (cmake_version(c) or (0,)) >= MIN_CMAKE), None)
    if not cmake:
        found = ", ".join(f"{c} ({'.'.join(map(str, cmake_version(c) or ()))})" for c in candidates if c) or "none"
        raise ToolchainError(f"CMake {MIN_CMAKE[0]}.{MIN_CMAKE[1]}+ is required (found: {found}).")

    ninja = shutil.which("ninja") or shutil.which("ninja", path=env.get("PATH") or env.get("Path"))
    if not ninja:
        raise ToolchainError("Ninja was not found: install it, or the Visual Studio CMake tools.")

    return Toolchain(vs_path=vs, env=env, cmake=cmake, ninja=ninja)
