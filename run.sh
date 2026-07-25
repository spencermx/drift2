#!/bin/sh
# Build (if needed) and run drift under Wine on macOS.
# Usage: ./run.sh
set -e
cd "$(dirname "$0")"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "Missing the Windows cross-compiler. Install it with:"
    echo "  brew install mingw-w64"
    exit 1
fi

if ! command -v wine >/dev/null 2>&1; then
    echo "Missing Wine. Install it with:"
    echo "  brew install --cask wine-stable"
    echo "  xattr -dr com.apple.quarantine '/Applications/Wine Stable.app'"
    exit 1
fi

# Rebuild only when the source is newer than the last build
if [ ! -f drift.exe ] || [ drift.c -nt drift.exe ]; then
    echo "Building drift.exe..."
    x86_64-w64-mingw32-gcc drift.c -o drift.exe
fi

# Point drift's home jump (~) and workspace storage at the real macOS home;
# without this Wine's fake profile dir (~/.wine/drive_c/users/...) is used
HOME_WIN="Z:$(printf '%s' "$HOME" | tr '/' '\\')"
DRIFT_HOME="$HOME_WIN"
export DRIFT_HOME

# Sessions live in the real macOS ~/.claude when running under Wine
DRIFT_CLAUDE_DIR="$HOME_WIN\\.claude"
export DRIFT_CLAUDE_DIR

# Workspace folder lists are written host-style (/Users/...) so the macOS
# claude can read them; drift translates to Z:\ form internally
DRIFT_HOST_DRIVE="Z:"
export DRIFT_HOST_DRIVE

# Claude launch handoff: a Windows process can't spawn the macOS claude, so
# drift writes its launch request to this file and exits; we run claude on
# the host and then restart drift back into the claude browser
LAUNCH_UNIX="$HOME/.drift/claude_launch"
DRIFT_LAUNCH_FILE="$HOME_WIN\\.drift\\claude_launch"
export DRIFT_LAUNCH_FILE
mkdir -p "$HOME/.drift"
rm -f "$LAUNCH_UNIX"

DRIFT_ARGS=""
while :; do
    wine drift.exe $DRIFT_ARGS
    [ -f "$LAUNCH_UNIX" ] || break

    ANCHOR_WIN=$(sed -n 1p "$LAUNCH_UNIX" | tr -d '\r')
    CLAUDE_ARGS=$(sed -n 2p "$LAUNCH_UNIX" | tr -d '\r')
    rm -f "$LAUNCH_UNIX"

    case "$ANCHOR_WIN" in
        [Zz]:*) ANCHOR_UNIX=$(printf '%s' "$ANCHOR_WIN" | cut -c3- | tr '\\' '/');;
        *) echo "drift: cannot launch claude in non-host path: $ANCHOR_WIN"; break;;
    esac

    (cd "$ANCHOR_UNIX" && claude $CLAUDE_ARGS) || true
    DRIFT_ARGS="-c"
done
