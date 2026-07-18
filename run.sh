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

# Point drift's home jump (~) at the real macOS home directory; without
# this it would land in Wine's fake profile dir (~/.wine/drive_c/users/...)
DRIFT_HOME="Z:$(printf '%s' "$HOME" | tr '/' '\\')"
export DRIFT_HOME

exec wine drift.exe
