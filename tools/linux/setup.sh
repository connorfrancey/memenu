#!/usr/bin/env bash
# Sets up a Linux machine (Ubuntu 22.04/24.04, the Steam Deck, other x86-64 distros) to build and test the menu:
# the 32-bit Windows DLL is cross-compiled with clang-cl / lld-link against Microsoft's CRT and Windows SDK (downloaded
# by xwin), the unit tests run under Wine, and the in-game test runs the game through Steam's Proton.
#
#     tools/linux/setup.sh                      everything in ~/.local/share/memenu-toolchain, no root needed
#     tools/linux/setup.sh --system             LLVM and Wine from apt instead (asks for sudo)
#     tools/linux/setup.sh --accept-msvc-license   don't ask about Microsoft's license (xwin's download needs it)
#     tools/linux/setup.sh --check              only report what is installed
#
# Afterwards: `source ~/.local/share/memenu-toolchain/env.sh` (the script offers to add it to ~/.bashrc), then
# `uv run memenu build`, `uv run memenu test` and `uv run memenu game-test` work as on Windows. See
# doc/linux-development.md.

set -euo pipefail

LLVM_VERSION=21.1.8
XWIN_VERSION=0.10.0
WINE_VERSION=11.20            # Kron4ek/Wine-Builds, the "wow64" build: runs 32-bit programs without i386 packages
PREFIX="${MEMENU_TOOLCHAIN:-$HOME/.local/share/memenu-toolchain}"

SYSTEM=0
ACCEPT_LICENSE=0
CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --system) SYSTEM=1 ;;
        --accept-msvc-license) ACCEPT_LICENSE=1 ;;
        --check) CHECK_ONLY=1 ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

if [ -t 1 ]; then
    BOLD=$'\e[1m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; RED=$'\e[31m'; RESET=$'\e[0m'
else
    BOLD=""; GREEN=""; YELLOW=""; RED=""; RESET=""
fi
step() { echo "${BOLD}==> $*${RESET}"; }
ok()   { echo "    ${GREEN}ok${RESET} $*"; }
warn() { echo "    ${YELLOW}!${RESET} $*"; }
die()  { echo "${RED}error:${RESET} $*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || die "this script is for Linux (on Windows use Visual Studio: see README.md)"
[ "$(uname -m)" = x86_64 ] || die "an x86-64 machine is needed (got $(uname -m))"
command -v curl >/dev/null || die "curl is needed (sudo apt install curl)"
command -v tar >/dev/null || die "tar is needed"

mkdir -p "$PREFIX/bin"
download() {    # url file
    curl -fL --retry 3 --progress-bar -o "$2.part" "$1" && mv "$2.part" "$2"
}

# ---- Report ---------------------------------------------------------------------------------------------------------
report() {
    # shellcheck disable=SC1091
    [ -f "$PREFIX/env.sh" ] && . "$PREFIX/env.sh"
    local missing=0
    for tool in uv cmake ninja clang-cl lld-link llvm-rc wine; do
        if command -v "$tool" >/dev/null; then ok "$tool: $(command -v "$tool")"; else warn "$tool: missing"; missing=1; fi
    done
    if [ -f "${MEMENU_XWIN_DIR:-$PREFIX/xwin}/crt/lib/x86/libcmt.lib" ]; then ok "MSVC CRT and Windows SDK (x86): ${MEMENU_XWIN_DIR:-$PREFIX/xwin}"
    else warn "MSVC CRT and Windows SDK (x86): missing"; missing=1; fi
    return $missing
}
if [ $CHECK_ONLY = 1 ]; then
    step "Toolchain in $PREFIX"
    report && echo "${GREEN}Everything is installed.${RESET}" || echo "Run tools/linux/setup.sh to install what is missing."
    exit 0
fi

# ---- uv (runs the project's Python tooling), CMake, Ninja ----------------------------------------------------------
step "uv, CMake and Ninja"
if ! command -v uv >/dev/null && [ ! -x "$HOME/.local/bin/uv" ]; then
    curl -LsSf https://astral.sh/uv/install.sh | env UV_NO_MODIFY_PATH=1 sh
fi
UV="$(command -v uv || echo "$HOME/.local/bin/uv")"
"$UV" tool install --quiet "cmake>=4.2" >/dev/null 2>&1 || "$UV" tool upgrade --quiet cmake
"$UV" tool install --quiet ninja >/dev/null 2>&1 || true
ok "$("$HOME/.local/bin/cmake" --version | head -1), ninja $("$HOME/.local/bin/ninja" --version)"

# ---- LLVM: clang-cl, lld-link, llvm-rc, llvm-mt ---------------------------------------------------------------------
step "LLVM (clang-cl, lld-link)"
LLVM_DIR="$PREFIX/llvm"
if [ $SYSTEM = 1 ]; then
    sudo apt-get update -qq
    sudo apt-get install -y clang-19 lld-19 llvm-19
    LLVM_DIR=/usr/lib/llvm-19
    # Ubuntu's packages have no clang-cl / lld-link names: link them next to the others
    for pair in clang-cl:clang lld-link:lld; do
        [ -e "$LLVM_DIR/bin/${pair%%:*}" ] || ln -sf "$LLVM_DIR/bin/${pair##*:}" "$PREFIX/bin/${pair%%:*}"
    done
elif [ ! -x "$LLVM_DIR/bin/clang-cl" ] || ! "$LLVM_DIR/bin/clang" --version | grep -q "$LLVM_VERSION"; then
    archive="$PREFIX/LLVM-$LLVM_VERSION-Linux-X64.tar.xz"
    download "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/LLVM-$LLVM_VERSION-Linux-X64.tar.xz" "$archive"
    rm -rf "$LLVM_DIR" && mkdir -p "$LLVM_DIR"
    tar -xf "$archive" -C "$LLVM_DIR" --strip-components=1
    rm -f "$archive"
fi
ok "$("$LLVM_DIR/bin/clang" --version | head -1)"

# ---- MSVC CRT + Windows SDK (xwin) --------------------------------------------------------------------------------
step "Microsoft's CRT and Windows SDK, x86 (xwin)"
XWIN_DIR="$PREFIX/xwin"
if [ ! -f "$XWIN_DIR/crt/lib/x86/libcmtd.lib" ]; then
    if [ $ACCEPT_LICENSE = 0 ]; then
        echo "    xwin downloads Microsoft's C/C++ runtime and the Windows SDK, under Microsoft's license:"
        echo "    https://go.microsoft.com/fwlink/?LinkId=2086102"
        read -r -p "    Do you accept it? [y/N] " answer
        [[ "$answer" =~ ^[Yy] ]] || die "not accepted: the Windows DLL can't be built without them"
    fi
    if [ ! -x "$PREFIX/bin/xwin" ]; then
        archive="$PREFIX/xwin.tar.gz"
        download "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz" "$archive"
        tar -xzf "$archive" -C "$PREFIX/bin" --strip-components=1 --wildcards '*/xwin'
        rm -f "$archive"
    fi
    # x86 only, with the debug CRT (Debug builds link the static debug runtime, /MTd)
    "$PREFIX/bin/xwin" --accept-license --arch x86 --cache-dir "$PREFIX/xwin-cache" splat \
        --include-debug-libs --output "$XWIN_DIR"
    rm -rf "$PREFIX/xwin-cache"
fi
ok "$XWIN_DIR"

# ---- Wine (runs the unit tests; the game itself runs in Steam's Proton) ---------------------------------------------
step "Wine"
WINE_DIR="$PREFIX/wine"
if [ $SYSTEM = 1 ]; then
    sudo dpkg --add-architecture i386
    sudo apt-get update -qq
    sudo apt-get install -y wine wine32:i386
    WINE_DIR=/usr
elif [ ! -x "$WINE_DIR/bin/wine" ]; then
    archive="$PREFIX/wine.tar.xz"
    download "https://github.com/Kron4ek/Wine-Builds/releases/download/$WINE_VERSION/wine-$WINE_VERSION-amd64-wow64.tar.xz" "$archive"
    rm -rf "$WINE_DIR" && mkdir -p "$WINE_DIR"
    tar -xf "$archive" -C "$WINE_DIR" --strip-components=1
    rm -f "$archive"
fi
# A prefix of its own for the tests, made once (Wine sets it up on first use)
export WINEPREFIX="$PREFIX/wineprefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
[ -d "$WINEPREFIX" ] || "$WINE_DIR/bin/wineboot" -i >/dev/null 2>&1 || true
ok "$("$WINE_DIR/bin/wine" --version)"

# ---- env.sh ---------------------------------------------------------------------------------------------------------
cat > "$PREFIX/env.sh" <<EOF
# Mirror's Edge Menu toolchain (tools/linux/setup.sh). Source it: . "$PREFIX/env.sh"
export MEMENU_TOOLCHAIN="$PREFIX"
export MEMENU_LLVM_DIR="$LLVM_DIR"
export MEMENU_XWIN_DIR="$XWIN_DIR"
export MEMENU_WINE_DIR="$WINE_DIR"
export WINEPREFIX="\${MEMENU_WINEPREFIX:-$PREFIX/wineprefix}"
export WINEDEBUG="\${WINEDEBUG:--all}"
case ":\$PATH:" in *":$PREFIX/bin:"*) ;; *) export PATH="$PREFIX/bin:$LLVM_DIR/bin:$WINE_DIR/bin:\$HOME/.local/bin:\$PATH" ;; esac
EOF
ok "$PREFIX/env.sh"

step "Check"
report || die "something is still missing (see above)"

line=". \"$PREFIX/env.sh\""
if ! grep -qF "$line" "$HOME/.bashrc" 2>/dev/null; then
    if [ -t 0 ]; then
        read -r -p "Add it to ~/.bashrc, so every new shell has it? [Y/n] " answer
        if [[ ! "$answer" =~ ^[Nn] ]]; then echo "$line" >> "$HOME/.bashrc"; ok "added to ~/.bashrc"; fi
    else
        echo "Add this to your shell's startup file: $line"
    fi
fi
echo
echo "${GREEN}Done.${RESET} In a new shell (or after: $line):"
echo "    uv run memenu build          # cmake-build-linux-x86-debug/medge_menu.dll"
echo "    uv run memenu test           # unit tests, under Wine"
echo "    uv run memenu game-test      # the in-game self-test, through Steam / Proton"
