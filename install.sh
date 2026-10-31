#!/usr/bin/env bash
# Installs (or updates, or removes) Mirror's Edge Menu on Linux, the Steam Deck and macOS, where Mirror's Edge runs
# through Proton (Steam), Wine, Lutris, CrossOver or Whisky. On Windows it hands over to install.ps1.
#
# Install or update:
#     curl -fsSL https://raw.githubusercontent.com/connorfrancey/memenu/master/install.sh | bash
#
# With options:
#     curl -fsSL https://raw.githubusercontent.com/connorfrancey/memenu/master/install.sh | bash -s -- --uninstall
#     curl -fsSL https://raw.githubusercontent.com/connorfrancey/memenu/master/install.sh | bash -s -- --game-dir "/path/to/mirrors edge"
#
# Options:
#     --uninstall          remove the menu (your settings are kept)
#     --remove-settings    with --uninstall: also delete the menu's config, themes and logs
#     --game-dir DIR       the game folder (or MirrorsEdge.exe), when it isn't found automatically
#     --version X.Y.Z      a release version instead of the latest one
#     -h, --help           show this
#
# What it does (the same as memenu_installer.exe on Windows):
#     <game>/Binaries/dinput8.dll              the loader, which loads the menu with the game
#     <game>/Binaries/memenu/medge_menu.dll    the menu (its settings and logs go in this folder too)
# A dinput8.dll from another mod is kept as dinput8.dll.memenu-backup and put back on uninstall.
#
# Wine and Proton use their own dinput8.dll unless told otherwise, so the game needs the override
# dinput8=n,b. For Steam this script tells you the launch option to add; for other Wine prefixes it sets it.

set -euo pipefail

REPO="connorfrancey/memenu"
ASSET="memenu-files.zip"
MARKER="memenu-loader:"
BACKUP="dinput8.dll.memenu-backup"
LAUNCH_OPTION='WINEDLLOVERRIDES="dinput8=n,b" %command%'

# ---- Output ------------------------------------------------------------------------------------------------------
if [ -t 1 ]; then
    BOLD=$'\e[1m'; DIM=$'\e[2m'; RED=$'\e[31m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; CYAN=$'\e[36m'; RESET=$'\e[0m'
else
    BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; CYAN=""; RESET=""
fi
step() { printf '  %s>%s %s\n' "$CYAN" "$RESET" "$1"; }
note() { printf '  %s%s%s\n' "$DIM" "$1" "$RESET"; }
warn() { printf '  %s%s%s\n' "$YELLOW" "$1" "$RESET"; }
fail() { printf '\n  %s%s%s\n\n' "$RED" "$1" "$RESET" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage: install.sh [--uninstall [--remove-settings]] [--game-dir DIR] [--version X.Y.Z]

  --uninstall          remove the menu (your settings are kept)
  --remove-settings    with --uninstall: also delete the menu's config, themes and logs
  --game-dir DIR       the game folder (or MirrorsEdge.exe), when it isn't found automatically
  --version X.Y.Z      a release version instead of the latest one

Through curl:  curl -fsSL https://raw.githubusercontent.com/connorfrancey/memenu/master/install.sh | bash -s -- --uninstall
EOF
}

# ---- Arguments ---------------------------------------------------------------------------------------------------
UNINSTALL=0; REMOVE_SETTINGS=0; GAME_DIR=""; VERSION=""
while [ $# -gt 0 ]; do
    case "$1" in
        --uninstall) UNINSTALL=1 ;;
        --remove-settings) REMOVE_SETTINGS=1 ;;
        --game-dir) [ $# -ge 2 ] || fail "--game-dir needs a folder."; GAME_DIR="$2"; shift ;;
        --game-dir=*) GAME_DIR="${1#*=}" ;;
        --version) [ $# -ge 2 ] || fail "--version needs a version, such as 2.0.0."; VERSION="${2#v}"; shift ;;
        --version=*) VERSION="${1#*=}"; VERSION="${VERSION#v}" ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

printf '\n  %sMirror'\''s Edge Menu%s\n  %shttps://github.com/%s%s\n\n' "$BOLD" "$RESET" "$DIM" "$REPO" "$RESET"

# ---- Windows: install.ps1 does it there --------------------------------------------------------------------------
case "${MEMENU_TEST_OS:-$(uname -s)}" in     # MEMENU_TEST_OS=Linux: test the Linux path anywhere
    MINGW*|MSYS*|CYGWIN*)
        step "This is Windows: running install.ps1 instead"
        ps_args=""
        [ "$UNINSTALL" = 1 ] && ps_args="$ps_args -Uninstall"
        [ "$REMOVE_SETTINGS" = 1 ] && ps_args="$ps_args -RemoveSettings"
        [ -n "$VERSION" ] && ps_args="$ps_args -Version '$VERSION'"
        if [ -n "$GAME_DIR" ]; then
            gd="$(cygpath -w "$GAME_DIR" 2>/dev/null || echo "$GAME_DIR")"
            ps_args="$ps_args -GameDir '${gd//\'/\'\'}'"     # ' doubled, for "Mirror's Edge" in a PowerShell string
        fi
        exec powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
            "& ([scriptblock]::Create((irm https://raw.githubusercontent.com/$REPO/master/install.ps1)))$ps_args"
        ;;
    Linux|Darwin) ;;
    *) fail "This script is for Linux, the Steam Deck and macOS (and hands over to install.ps1 on Windows)." ;;
esac

# ---- Tools -------------------------------------------------------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

download() {   # url file
    if have curl; then curl -fsSL --retry 2 -o "$2" "$1"
    elif have wget; then wget -q -O "$2" "$1"
    else fail "Neither curl nor wget is installed. Install one of them and run this again."
    fi
}

sha256() {
    if have sha256sum; then sha256sum "$1" | cut -d' ' -f1
    elif have shasum; then shasum -a 256 "$1" | cut -d' ' -f1
    else fail "No sha256sum or shasum to check the download with."
    fi
}

unpack() {   # zip dir
    if have unzip; then unzip -q -o "$1" -d "$2"
    elif have bsdtar; then bsdtar -xf "$1" -C "$2"
    elif have python3; then python3 -m zipfile -e "$1" "$2"
    else fail "No unzip, bsdtar or python3 to unpack the download with. Install unzip and run this again."
    fi
}

is_our_loader() { [ -f "$1" ] && grep -aq "$MARKER" "$1"; }

game_running() { pgrep -if 'MirrorsEdge\.exe' >/dev/null 2>&1; }

# ---- Find the game -----------------------------------------------------------------------------------------------
# Prints the folder with MirrorsEdge.exe for a game folder, a Binaries folder or the exe itself.
binaries_of() {
    local p="$1"
    [ -f "$p" ] && p="$(dirname "$p")"
    for c in "$p/Binaries" "$p"; do
        [ -f "$c/MirrorsEdge.exe" ] && { (cd "$c" && pwd); return 0; }
    done
    return 1
}

steam_roots() {
    local home="$HOME"
    printf '%s\n' \
        "$home/.steam/steam" "$home/.steam/root" "$home/.local/share/Steam" \
        "$home/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
        "$home/snap/steam/common/.local/share/Steam" \
        "$home/Library/Application Support/Steam"
}

# Every Steam library folder: the roots themselves and the "path" entries of their libraryfolders.vdf.
steam_libraries() {
    local root vdf
    while IFS= read -r root; do
        [ -d "$root" ] || continue
        printf '%s\n' "$root"
        for vdf in "$root/steamapps/libraryfolders.vdf" "$root/config/libraryfolders.vdf"; do
            [ -f "$vdf" ] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf"
        done
    done < <(steam_roots)
}

# Wine prefixes (plain Wine, Lutris, Bottles, Heroic, CrossOver, Whisky): any folder with a drive_c in it.
wine_prefixes() {
    local home="$HOME" d
    for d in "$home/.wine" "$home/Games"/* "$home/.local/share/lutris/prefixes"/* \
             "$home/.var/app/com.usebottles.bottles/data/bottles/bottles"/* "$home/.local/share/bottles/bottles"/* \
             "$home/Games/Heroic/Prefixes"/* \
             "$home/Library/Application Support/CrossOver/Bottles"/* \
             "$home/Library/Containers/com.isaacmarovitz.Whisky/Bottles"/*; do
        [ -d "$d/drive_c" ] && printf '%s\n' "$d"
    done
    return 0
}

FOUND_VIA=""     # steam | wine (worked out from the path once the game is found)
WINE_PREFIX=""

find_game() {
    local lib dir prefix exe
    while IFS= read -r lib; do
        dir="$lib/steamapps/common/mirrors edge"
        if dir="$(binaries_of "$dir" 2>/dev/null)"; then printf '%s\n' "$dir"; return 0; fi
    done < <(steam_libraries | awk '!seen[$0]++')

    while IFS= read -r prefix; do
        exe="$(find "$prefix/drive_c" -maxdepth 7 -type f -name 'MirrorsEdge.exe' 2>/dev/null | head -n 1 || true)"
        if [ -n "$exe" ]; then
            dirname "$exe"; return 0
        fi
    done < <(wine_prefixes)
    return 1
}

if [ -n "$GAME_DIR" ]; then
    BIN="$(binaries_of "$GAME_DIR")" || fail "MirrorsEdge.exe is not in $GAME_DIR (or its Binaries folder)."
    case "$BIN" in
        */steamapps/common/*) FOUND_VIA="steam" ;;
        */drive_c/*) FOUND_VIA="wine"; WINE_PREFIX="${BIN%%/drive_c/*}" ;;
    esac
else
    step "Looking for Mirror's Edge"
    BIN="$(find_game)" || fail "Mirror's Edge was not found. Run this again with --game-dir \"<your game folder>\".
  Steam: right click the game > Manage > Browse local files shows the folder."
    case "$BIN" in
        */drive_c/*) FOUND_VIA="wine"; WINE_PREFIX="${BIN%%/drive_c/*}" ;;
        *) FOUND_VIA="steam" ;;
    esac
fi
note "Game: $BIN"

game_running && fail "Mirror's Edge is running. Close the game, then run this again."
[ -w "$BIN" ] || fail "Cannot write to $BIN. Check that folder's permissions."

# ---- Wine DLL override -------------------------------------------------------------------------------------------
set_wine_override() {   # add (1) or remove (0)
    [ "$FOUND_VIA" = "wine" ] && [ -n "$WINE_PREFIX" ] || return 1
    have wine || return 1
    case "$WINE_PREFIX" in */CrossOver/*|*/Whisky/*) return 1 ;; esac   # Those bottles have their own wine
    if [ "$1" = 1 ]; then
        WINEPREFIX="$WINE_PREFIX" WINEDEBUG=-all wine reg add 'HKCU\Software\Wine\DllOverrides' \
            /v dinput8 /d native,builtin /f >/dev/null 2>&1
    else
        WINEPREFIX="$WINE_PREFIX" WINEDEBUG=-all wine reg delete 'HKCU\Software\Wine\DllOverrides' \
            /v dinput8 /f >/dev/null 2>&1
    fi
}

# ---- Uninstall ---------------------------------------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
    step "Removing Mirror's Edge Menu"
    is_our_loader "$BIN/dinput8.dll" && rm -f "$BIN/dinput8.dll"
    if [ -f "$BIN/$BACKUP" ] && [ ! -e "$BIN/dinput8.dll" ]; then
        mv "$BIN/$BACKUP" "$BIN/dinput8.dll"
        note "Put back the other mod's dinput8.dll."
    fi
    if [ "$REMOVE_SETTINGS" = 1 ]; then
        rm -rf "$BIN/memenu"
        printf '\n  %sMirror'\''s Edge Menu was removed, with its settings.%s\n' "$GREEN" "$RESET"
    else
        rm -f "$BIN/memenu/medge_menu.dll"
        printf '\n  %sMirror'\''s Edge Menu was removed.%s Your settings were kept in %s/memenu.\n' \
            "$GREEN" "$RESET" "$BIN"
    fi
    if [ ! -e "$BIN/dinput8.dll" ]; then
        if [ "$FOUND_VIA" = "steam" ]; then
            note "You can take WINEDLLOVERRIDES=... out of the game's Steam launch options again."
        else
            set_wine_override 0 || true
        fi
    fi
    echo
    exit 0
fi

# ---- Download ----------------------------------------------------------------------------------------------------
if [ -n "${MEMENU_RELEASE_URL:-}" ]; then BASE="$MEMENU_RELEASE_URL"     # Testing: a folder with the release files
elif [ -n "$VERSION" ]; then BASE="https://github.com/$REPO/releases/download/v$VERSION"
else BASE="https://github.com/$REPO/releases/latest/download"; fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/memenu.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

step "Downloading Mirror's Edge Menu${VERSION:+ (version $VERSION)}"
download "$BASE/$ASSET" "$WORK/$ASSET" && download "$BASE/SHA256SUMS.txt" "$WORK/SHA256SUMS.txt" ||
    fail "Download failed. Check your internet connection${VERSION:+ and that version $VERSION exists}.
  (Version 2.0.0 and older have no $ASSET: use 2.0.1-dev or newer.)"

step "Checking the download"
expected="$(tr -d '\r' < "$WORK/SHA256SUMS.txt" | awk -v f="$ASSET" '$2 == f || $2 == "*" f { print tolower($1); exit }')"
[ -n "$expected" ] || fail "The release has no checksum for $ASSET."
[ "$(sha256 "$WORK/$ASSET")" = "$expected" ] ||
    fail "The download is damaged or was changed (checksum mismatch). Nothing was installed."

mkdir "$WORK/files"
unpack "$WORK/$ASSET" "$WORK/files"
[ -f "$WORK/files/dinput8.dll" ] && [ -f "$WORK/files/memenu/medge_menu.dll" ] ||
    fail "The download doesn't have the expected files. Nothing was installed."

# ---- Install -----------------------------------------------------------------------------------------------------
step "Installing into the game folder"
updating=0
is_our_loader "$BIN/dinput8.dll" && [ -f "$BIN/memenu/medge_menu.dll" ] && updating=1
if [ -e "$BIN/dinput8.dll" ] && ! is_our_loader "$BIN/dinput8.dll"; then
    if [ ! -e "$BIN/$BACKUP" ]; then mv "$BIN/dinput8.dll" "$BIN/$BACKUP"; fi
    note "Another mod's dinput8.dll was saved as $BACKUP and is put back when you uninstall."
fi
mkdir -p "$BIN/memenu"
cp -f "$WORK/files/memenu/medge_menu.dll" "$BIN/memenu/medge_menu.dll"
cp -f "$WORK/files/dinput8.dll" "$BIN/dinput8.dll"

echo
if [ "$updating" = 1 ]; then printf '  %sUpdated!%s\n' "$GREEN" "$RESET"
else printf '  %sInstalled!%s\n' "$GREEN" "$RESET"; fi

# ---- The override ------------------------------------------------------------------------------------------------
if [ "$FOUND_VIA" = "steam" ]; then
    cat <<EOF

  ${BOLD}One more step in Steam${RESET} (Proton ignores the menu without it, and only Steam can change it):
    1. In your Library, right click ${BOLD}Mirror's Edge${RESET} > ${BOLD}Properties${RESET}.
    2. Under ${BOLD}General > Launch Options${RESET}, paste:

         ${CYAN}${LAUNCH_OPTION}${RESET}

  (Only once: updates keep it. On a Steam Deck, do this in Desktop Mode or from the game's gear icon > Properties.)
EOF
elif set_wine_override 1; then
    note "Set dinput8 to native in the Wine prefix $WINE_PREFIX."
else
    cat <<EOF

  ${BOLD}One more step:${RESET} tell Wine to use the game's dinput8.dll.
    - Lutris / Bottles / Heroic: add the environment variable  ${CYAN}WINEDLLOVERRIDES=dinput8=n,b${RESET}
      in the game's settings.
    - CrossOver / Whisky / plain Wine: run winecfg (Wine Configuration) for the game's bottle, open
      ${BOLD}Libraries${RESET}, add ${BOLD}dinput8${RESET} and set it to ${BOLD}Native then Builtin${RESET}.
EOF
fi

cat <<EOF

  Then start Mirror's Edge the way you always do.
  In the game: ${BOLD}INSERT${RESET} opens the menu, ${BOLD}END${RESET} unloads it.
  ${DIM}Run this command again any time to update. Running the game through Wine/Proton is less tested than Windows:
  if the menu doesn't show up, look at $BIN/memenu/loader.log and logs/memenu.log there.${RESET}

EOF
