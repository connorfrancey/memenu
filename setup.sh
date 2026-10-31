#!/usr/bin/env bash
# setup.sh - start here on Linux, the Steam Deck or macOS:  bash setup.sh
#
#   bash setup.sh              asks what you want to do
#   bash setup.sh install      install the menu into Mirror's Edge (the latest release), to play with it
#   bash setup.sh uninstall    remove it from the game again (your settings are kept)
#
# Building the menu needs Windows (32-bit MSVC), so there is no developer setup here: on Windows, run setup.bat.
# Installing is done by install.sh, next to this script (see it for --game-dir and --version).

set -euo pipefail
cd "$(dirname "$0")"

case "${MEMENU_TEST_OS:-$(uname -s)}" in
    MINGW*|MSYS*|CYGWIN*) exec cmd.exe //c setup.bat "$@" ;;     # Git Bash on Windows: setup.bat does it there
esac

if [ -t 1 ]; then BOLD=$'\e[1m'; DIM=$'\e[2m'; RESET=$'\e[0m'; else BOLD=""; DIM=""; RESET=""; fi

action="${1:-}"
[ $# -gt 0 ] && shift
if [ -z "$action" ]; then
    printf '\n  %sMirror'\''s Edge Menu%s\n  %shttps://github.com/connorfrancey/memenu%s\n\n' "$BOLD" "$RESET" "$DIM" "$RESET"
    echo "  What do you want to do?"
    echo
    echo "    1  Install the menu into Mirror's Edge (or update it)"
    echo "    2  Remove the menu from the game"
    echo "    Q  Quit"
    echo
    printf "  Press 1, 2 or Q, then Enter: "
    read -r choice || choice="q"
    case "$choice" in
        1) action="install" ;;
        2) action="uninstall" ;;
        *) exit 0 ;;
    esac
fi

case "$action" in
    install) exec bash ./install.sh "$@" ;;
    uninstall) exec bash ./install.sh --uninstall "$@" ;;
    dev)
        echo "  Building the menu needs Windows (Visual Studio's 32-bit C++ tools): run setup.bat there."
        exit 1 ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//' ;;
    *) echo "  Unknown option \"$action\". Use: bash setup.sh [install | uninstall]"; exit 2 ;;
esac
