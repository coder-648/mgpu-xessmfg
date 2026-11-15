#!/usr/bin/env bash
# ============================================================================
#  cp_mode.sh - switch Cyberpunk 2077 between capture configurations
#
#  Written for bisecting "the game does not start": each mode changes ONE thing,
#  so the run that starts tells us which layer is responsible.
#
#    full     FgSniff + SlInput + FgNgxHooks + FgCut=6 + own host   (the target)
#    nocut    same but FgCut=0        -> is the FG break the blocker?
#    nofg     no FG lane at all       -> are the sl/NGX FG hooks the blocker?
#    nohost   Host=0 in the proxy     -> is the D3D12 wrapper host the blocker?
#    noproxy  proxy does nothing      -> is ArchSpoof/MinArchPatch the blocker?
# ============================================================================
set -uo pipefail
GAME="${MGPU_CP2077:-/mnt/e/SteamLibrary/steamapps/common/Cyberpunk 2077/bin/x64}"
MODE="${1:?usage: cp_mode.sh full|nocut|nofg|nohost|noproxy}"
[ -d "$GAME" ] || { echo "game dir not found: $GAME"; exit 1; }

host=1
case "$MODE" in
    full)    sniff=1; slin=1; ngx=1; cut=6 ;;
    nocut)   sniff=1; slin=1; ngx=1; cut=0 ;;
    nofg)    sniff=0; slin=0; ngx=0; cut=0 ;;
    nohost)  sniff=1; slin=1; ngx=1; cut=0; host=0 ;;
    nopatch) sniff=0; slin=0; ngx=0; cut=0; host=1 ;;   # ArchSpoof on, MinArchPatch off
    noproxy) sniff=0; slin=0; ngx=0; cut=0; host=1 ;;   # both proxy patches off
    *) echo "unknown mode: $MODE"; exit 1 ;;
esac

printf '[proxy]\r\nArchSpoof=%d\r\nMinArchPatch=%d\r\nHost=%d\r\n' \
    $([ "$MODE" = noproxy ] && echo 0 || echo 1) \
    $([ "$MODE" = noproxy ] && echo 0 || echo 1) \
    "$host" > "$GAME/mgpu_proxy.ini"

printf '%s\r\n' \
    '[capture]' \
    'Log=1' \
    'Color=0' \
    'Input=auto' \
    "SlInput=$slin" \
    'StreamlineCamera=1' \
    "FgSniff=$sniff" \
    "FgNgxHooks=$ngx" \
    "FgCut=$cut" \
    'SkipDepth=0' \
    'DepthDirect=1' \
    'StreamPercent=100' \
    'MvNormalize=0' \
    'LoaderHook=1' > "$GAME/mgpu_capture.ini"

rm -f "$GAME/mgpu_capture.log" "$GAME/mgpu_proxy.log" "$GAME/mgpu_capture_host.log"
echo "[cp_mode] $MODE applied:"
echo "           proxy: $(tr -d '\r' < "$GAME/mgpu_proxy.ini" | tr '\n' ' ')"
echo "           capture: FgSniff=$sniff SlInput=$slin FgNgxHooks=$ngx FgCut=$cut"
echo "           logs cleared - start the game, then read bin\\x64\\mgpu_*.log"
