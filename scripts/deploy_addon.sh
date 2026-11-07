#!/usr/bin/env bash
# ===========================================================================
#  Copy build/game_capture.addon64 into the game, keeping one .bak.
#
#  The game keeps the DLL loaded, so this only works while the game is closed
#  (otherwise Windows refuses the overwrite with an I/O error).
#
#    ./scripts/deploy_addon.sh "<GAME_DIR>"      # folder that holds the game exe
#
#  There is no default path: pass the game's bin folder (see docs/ops/CONFIG.md).
# ===========================================================================
set -uo pipefail

GAMEDIR="${1:-}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/build/game_capture.addon64"

if [ -z "$GAMEDIR" ]; then
    echo "usage: deploy_addon.sh <game-bin-dir>    (the folder that contains the game .exe)"
    exit 1
fi

if [ ! -f "$SRC" ]; then
    echo "[deploy] missing $SRC - build it first (scripts/build_addon.sh src/game_capture game_capture)"
    exit 1
fi
if [ ! -d "$GAMEDIR" ]; then
    echo "[deploy] no such game dir: $GAMEDIR"
    exit 1
fi

cp -f "$GAMEDIR/game_capture.addon64" "$GAMEDIR/game_capture.addon64.bak" 2>/dev/null || true
# cp into these Steam libraries can report success without the bytes landing (9p/NTFS
# caching), so write through a redirect and verify with cmp.
cat "$SRC" > "$GAMEDIR/game_capture.addon64" 2>/dev/null
if ! cmp -s "$SRC" "$GAMEDIR/game_capture.addon64"; then
    echo "[deploy] copy FAILED (file locked? close the game and retry)"
    exit 3
fi

echo "[deploy] installed:"
md5sum "$GAMEDIR/game_capture.addon64" "$GAMEDIR/game_capture.addon64.bak" 2>/dev/null | sed 's/^/[deploy]   /'
echo "[deploy] now start the game, then run the bridge script"
