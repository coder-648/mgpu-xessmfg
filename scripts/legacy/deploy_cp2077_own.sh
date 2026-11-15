#!/usr/bin/env bash
# ============================================================================
#  deploy_cp2077_own.sh - make Cyberpunk 2077 the own-host test bed
#
#  CP2077 is now the only test environment. This puts the *own host* stack there
#  (no ReShade):
#
#     version.dll        our minimal proxy: ArchSpoof + MinArchPatch + Host=1
#     mgpu_capture.dll   capture core + own D3D12 wrapper host
#     mgpu_proxy.ini     the three switches above
#     mgpu_capture.ini   CP2077 capture config
#
#  ReShade is only renamed, never deleted: dxgi.dll -> dxgi.dll.reshade_off and
#  game_capture.addon64 -> game_capture.addon64.off. Both are restored by
#  scripts/restore_cp2077_reshade.sh, and dlssg_sm86's own proxy (version.dll.sm86)
#  is left untouched so the unlocker can be put back if DLSS-G refuses to init.
# ============================================================================
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${MGPU_CP2077:-/mnt/e/SteamLibrary/steamapps/common/Cyberpunk 2077/bin/x64}"

[ -d "$GAME" ] || { echo "game dir not found: $GAME"; exit 1; }
[ -f "$REPO/build/version.dll" ] || { echo "build/version.dll missing (run build_proxy.sh)"; exit 1; }
[ -f "$REPO/build/mgpu_capture.dll" ] || { echo "build/mgpu_capture.dll missing (run build_capture_dll.sh)"; exit 1; }

# ReShade out of the way (rename keeps the previous state recoverable).
[ -e "$GAME/dxgi.dll" ] && mv -f "$GAME/dxgi.dll" "$GAME/dxgi.dll.reshade_off"
[ -e "$GAME/game_capture.addon64" ] && mv -f "$GAME/game_capture.addon64" "$GAME/game_capture.addon64.off"

cp -f "$REPO/build/version.dll"       "$GAME/version.dll"
cp -f "$REPO/build/mgpu_capture.dll"  "$GAME/mgpu_capture.dll"

# The ini templates live in scripts/templates/ with a full explanation of every key. They are
# UTF-8 in the repo (so they can be read and diffed) and are converted to the ANSI/GBK the
# game's GetPrivateProfile* calls expect. Editing the template is the only supported way to
# change a default: a hand-written file has already cost us a whole evening once, when a
# six-line rewrite dropped Host=1 and the game ran with no capture at all.
for tpl in mgpu_capture mgpu_proxy; do
    iconv -f UTF-8 -t GBK "$REPO/scripts/templates/$tpl.ini" \
        | awk '{ printf "%s\r\n", $0 }' > "$GAME/$tpl.ini" || exit 1
done

# ROTATE, never delete: a log that disappears is evidence we cannot get back (that happened
# twice today - the game had just produced a clean run and the deploy wiped it).
STAMP=$(date +%H%M%S)
for f in mgpu_capture.log mgpu_capture_host.log mgpu_proxy.log; do
    [ -f "$GAME/$f" ] && mv -f "$GAME/$f" "$GAME/$f.prev-$STAMP"
done

echo "[deploy] CP2077 own host ready in:"
echo "         $GAME"
echo "         version.dll (minimal proxy, Host=1) + mgpu_capture.dll"
echo "         in-game: DLSS SR ON, Frame Generation ON"
echo "         then: C:\\temp\\mgpu\\bin\\run_cp_own.bat"
