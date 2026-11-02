:<<"::CMDLITERAL"
@echo off
rem ---------------------------------------------------------------------------------------------------------------
rem  setup.cmd - start here, on any system. Install or remove the menu, or set up for development.
rem
rem    Windows:        double-click setup.cmd, or:  setup.cmd [install | uninstall | dev]
rem    Linux / macOS:  bash setup.cmd [install | uninstall] [install.sh options, e.g. --game-dir DIR]
rem
rem    (no option)  asks what you want to do
rem    install      install the menu into Mirror's Edge (the latest release), or update it
rem    uninstall    remove it from the game again (your settings are kept)
rem    dev          Windows only: install what building needs (uv, CMake, Ninja, the VS C++ Build Tools) with
rem                 winget, then the project tool (tools/dev-setup.ps1)
rem
rem  The installing is done by install.ps1 (Windows) and install.sh (Linux, the Steam Deck, macOS) next to this file.
rem
rem  How this works: this one file is both a cmd.exe batch file and a bash script. cmd sees the first line as a label
rem  and runs this part up to "exit /b". bash sees ":" with a here-document that swallows this part, and runs what
rem  comes after ::CMDLITERAL. The file must keep LF line endings (.gitattributes), and this part must not use
rem  labels, goto or call :label: cmd finds labels unreliably in LF files.
rem ---------------------------------------------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
title Mirror's Edge Menu setup

rem Without options (how a double-click starts it): wait for a key at the end, so the window doesn't just close.
set "PAUSE_AT_END="
if "%~1"=="" set "PAUSE_AT_END=1"

echo.
echo   Mirror's Edge Menu
echo   https://github.com/connorfrancey/memenu
echo.

set "ACTION=%~1"
if "%ACTION%"=="" (
    echo   What do you want to do?
    echo.
    echo     1  Install the menu into Mirror's Edge, or update it
    echo     2  Set up this folder to build the menu yourself, for developers
    echo     3  Remove the menu from the game
    echo     Q  Quit
    echo.
    choice /c 123Q /n /m "  Press 1, 2, 3 or Q: "
    if errorlevel 4 (set "ACTION=quit") else if errorlevel 3 (set "ACTION=uninstall") else if errorlevel 2 (set "ACTION=dev") else (set "ACTION=install")
)

set "CODE=0"
if /i "%ACTION%"=="install" (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
    if errorlevel 1 set "CODE=1"
) else if /i "%ACTION%"=="uninstall" (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
    if errorlevel 1 set "CODE=1"
) else if /i "%ACTION%"=="dev" (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\dev-setup.ps1"
    if errorlevel 1 set "CODE=1"
) else if /i not "%ACTION%"=="quit" (
    echo   Unknown option "%ACTION%". Use: setup.cmd [install ^| uninstall ^| dev]
    set "CODE=2"
)

if defined PAUSE_AT_END (echo. & pause)
endlocal & exit /b %CODE%
::CMDLITERAL
# ---- Linux, the Steam Deck, macOS (and Git Bash on Windows) ------------------------------------------------------
# Run as `sh setup.cmd` or `./setup.cmd` (no shebang: line 1 belongs to cmd): make sure this is bash.
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi

set -euo pipefail
cd "$(dirname "$0")"

case "${MEMENU_TEST_OS:-$(uname -s)}" in
    # Git Bash on Windows: the cmd part does it there. .\ because cmd may not search the current folder
    # (NoDefaultCurrentDirectoryInExePath).
    MINGW*|MSYS*|CYGWIN*) exec cmd.exe //c '.\setup.cmd' "$@" ;;
esac

usage() {
    cat <<'EOF'
Usage: bash setup.cmd [install | uninstall] [options]

  (no option)  asks what you want to do
  install      install the menu into Mirror's Edge (the latest release), or update it
  uninstall    remove it from the game again (your settings are kept)

Options after install / uninstall go to install.sh: --game-dir DIR, --version X.Y.Z, --remove-settings.
Building the menu needs Windows (Visual Studio's 32-bit C++ tools): run setup.cmd dev there.
EOF
}

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
        echo "  Building the menu needs Windows (Visual Studio's 32-bit C++ tools): run setup.cmd dev there."
        exit 1 ;;
    -h|--help|help) usage ;;
    *) echo "  Unknown option \"$action\". Use: bash setup.cmd [install | uninstall]"; exit 2 ;;
esac
