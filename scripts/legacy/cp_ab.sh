#!/usr/bin/env bash
# cp_ab.sh <A|B|C|D>  - CP2077 capture A/B harness (2026-11-11)
#
#   A  capture OFF  (Host=0): pure game baseline
#   B  capture ON   MV + depth         126 MB/frame
#   C  capture ON   MV only            63 MB/frame
#   D  capture ON   MV + depth + HUDless ~193 MB/frame
#   E  capture ON   MV + HUDless            ~130 MB/frame   (colour without depth)
#   F  capture ON   HUDless only             ~67 MB/frame   (is colour alone expensive?)
#   G  capture ON   MV + depth + HUDless + UI (UI is not tagged by CP2077; kept for other titles)
#
# The only per-run difference is the capture. Nothing else (bridge, FgCut, DLSS mode)
# should change between runs, otherwise the comparison is meaningless.
set -uo pipefail
GAME="${MGPU_CP2077:-/mnt/e/SteamLibrary/steamapps/common/Cyberpunk 2077/bin/x64}"
MODE="${1:?usage: cp_ab.sh A|B|C|D}"

host=1; color=0; ui=0; skipdepth=0; skipmv=0
case "$MODE" in
    A) host=0 ;;
    B) color=0; ui=0; skipdepth=0 ;;
    C) color=0; ui=0; skipdepth=1 ;;
    D) color=1; ui=0; skipdepth=0 ;;
    E) color=1; ui=0; skipdepth=1 ;;
    F) color=1; ui=0; skipdepth=1; skipmv=1 ;;
    G) color=1; ui=1; skipdepth=0 ;;
    *) echo "unknown mode: $MODE"; exit 1 ;;
 esac

printf '[proxy]\r\nArchSpoof=1\r\nMinArchPatch=1\r\nHost=%d\r\nNoWrap=0\r\nPresentHook=0\r\nRawVtableHooks=0\r\nRecordList=0\r\nSwapchainWrap=1\r\n' "$host" > "$GAME/mgpu_proxy.ini"
printf '%s\r\n' '[capture]' 'Log=1' "Color=$color" 'ColorFormat=27' "Ui=$ui" 'UiFormat=0' \
    "SkipMv=$skipmv" 'Input=auto' "SlInput=$([ "$skipmv" = 1 ] && echo 0 || echo 1)" 'StreamlineCamera=1' 'FgSniff=1' 'FgNgxHooks=1' 'FgCut=6' \
    'FollowConsumer=0' "SkipDepth=$skipdepth" 'DepthDirect=1' 'MvFromUav=1' \
    'StreamPercent=100' 'MvNormalize=0' 'LoaderHook=1' \
    'FGVelocityValidNow=1' 'FGDepthValidNow=1' 'FGHudlessValidNow=1' > "$GAME/mgpu_capture.ini"

S=$(date +%H%M%S)
for f in mgpu_capture.log mgpu_capture_host.log; do
    [ -f "$GAME/$f" ] && mv -f "$GAME/$f" "$GAME/$f.prev-$S" 2>/dev/null
done
echo "[cp_ab] mode $MODE: host=$host color=$color ui=$ui skipdepth=$skipdepth"
echo "         start the game, play the same route, note the in-game fps counter"
