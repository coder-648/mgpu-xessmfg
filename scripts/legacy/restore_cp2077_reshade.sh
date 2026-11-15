#!/usr/bin/env bash
# ============================================================================
#  restore_cp2077_reshade.sh - put CP2077 back on the ReShade-hosted stack
#
#  Only needed when a comparison against the old path is wanted, or when the own
#  host is being debugged and the game must stay usable in the meantime.
# ============================================================================
set -uo pipefail
GAME="${MGPU_CP2077:-/mnt/e/SteamLibrary/steamapps/common/Cyberpunk 2077/bin/x64}"
[ -d "$GAME" ] || { echo "game dir not found: $GAME"; exit 1; }

[ -e "$GAME/dxgi.dll.reshade_off" ] && mv -f "$GAME/dxgi.dll.reshade_off" "$GAME/dxgi.dll"
[ -e "$GAME/game_capture.addon64.off" ] && mv -f "$GAME/game_capture.addon64.off" "$GAME/game_capture.addon64"
# The own-host DLLs stay on disk but stop loading: Host=0 in the proxy ini.
if [ -f "$GAME/mgpu_proxy.ini" ]; then
    printf '[proxy]\r\nArchSpoof=1\r\nMinArchPatch=1\r\nHost=0\r\n' > "$GAME/mgpu_proxy.ini"
fi
echo "[restore] ReShade back in place, own host disabled (Host=0)"
