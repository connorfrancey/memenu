#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["click>=8.1", "questionary>=2.1.1", "rich>=13.7", "zstandard>=0.23; python_version < '3.14'"]
# ///
"""Project manager for Mirror's Edge Menu: build, test, clean, install into the game, package.

    python memenu.py                        interactive menu: configure and build from a list
    python memenu.py --help
    python memenu.py build                  debug build of medge_menu.dll
    python memenu.py build -c release       release build
    python memenu.py build --with tests     also build an optional part (tests, installer, launcher)
    python memenu.py dev-test               build, start the game with the menu injected, report crashes
    python memenu.py minimal                only the D3D9 hook + ImGui demo window (INSERT)
    python memenu.py test                   build and run the unit tests
    python memenu.py verbose-build          build stage by stage, test every stage without the game
    python memenu.py clean --all            delete every build folder
    python memenu.py package                release installer -> dist/

`uv run memenu.py ...` works without any setup (uv installs click, rich and questionary from the header above).
With plain `python`, install them first: `pip install -r requirements.txt` (or `uv sync` and use the .venv).
questionary is only needed for the interactive menu.

This is a thin entry point: the commands live in tools/memenu_tools (also installed as `uv run memenu`).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))

try:
    from memenu_tools.cli import main
except ImportError as e:
    if e.name not in ("click", "rich"):
        raise
    sys.exit(f"memenu.py needs click and rich ({e.name} is missing).\n"
             f"Run it with `uv run memenu.py ...`, or install them: "
             f"{Path(sys.executable).name} -m pip install -r requirements.txt")

if __name__ == "__main__":
    main()
