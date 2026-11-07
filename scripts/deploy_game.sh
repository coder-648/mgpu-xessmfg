#!/usr/bin/env bash
# ===========================================================================
#  Install the ReShade add-on into any game directory (second game, UE5, ...).
#
#    ./scripts/deploy_game.sh <game-bin-dir> [exe-name]
#
#  What it does (all reversible):
#    1. copies ReShade 6.8 as dxgi.dll if the game has none (set RESHADE_SRC to a
#       game's existing dxgi.dll, or to a plain ReShade64.dll)
#    2. copies build/game_capture.addon64
#    3. writes game_capture.ini (Color=0, StreamPercent=100) unless one exists
#
#  Afterwards start the game with DLSS on (not TSR/FSR, frame gen off), then run the
#  matching bridge script with `--game <exe-name>`.
# ===========================================================================
set -uo pipefail

GAMEDIR="${1:-}"
EXE="${2:-}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ReShade 6.8 x64 host DLL. Point RESHADE_SRC at any 6.8 ReShade64.dll (a game's
# dxgi.dll works); there is no baked-in default path (see docs/ops/CONFIG.md).
RESHADE_SRC="${RESHADE_SRC:-}"
ADDON="$REPO/build/game_capture.addon64"

if [ -z "$GAMEDIR" ] || [ ! -d "$GAMEDIR" ]; then
    echo "usage: deploy_game.sh <game-bin-dir> [exe-name]"
    exit 1
fi
if [ ! -f "$ADDON" ]; then
    echo "[deploy] missing $ADDON - build it first"
    exit 1
fi

if [ -n "$EXE" ] && tasklist.exe 2>/dev/null | grep -qi "$(basename "$EXE" .exe)"; then
    echo "[deploy] $EXE is running - close the game first"
    exit 2
fi

# 1. ReShade needs to be loaded by the game; dxgi.dll is the usual proxy slot.
if [ ! -f "$GAMEDIR/dxgi.dll" ]; then
    if [ -f "$RESHADE_SRC" ]; then
        cp -f "$RESHADE_SRC" "$GAMEDIR/dxgi.dll" && echo "[deploy] ReShade -> dxgi.dll"
    else
        echo "[deploy] no ReShade source at $RESHADE_SRC (set RESHADE_SRC=...)"
        echo "[deploy] continuing, but the add-on will not load without ReShade"
    fi
else
    echo "[deploy] dxgi.dll already present - left alone"
fi

# 2. the add-on itself
cp -f "$GAMEDIR/game_capture.addon64" "$GAMEDIR/game_capture.addon64.bak" 2>/dev/null || true
# `cp -f` into these Steam libraries sometimes reports success while the bytes never land
# (9p/NTFS caching); writing through a redirect and comparing afterwards is reliable.
cat "$ADDON" > "$GAMEDIR/game_capture.addon64" 2>/dev/null
if cmp -s "$ADDON" "$GAMEDIR/game_capture.addon64"; then
    echo "[deploy] add-on installed"
else
    echo "[deploy] add-on copy FAILED (game running?) - keeping the existing one"
fi

# 3. config
if [ ! -f "$GAMEDIR/game_capture.ini" ]; then
    printf '[capture]\r\nColor=0\r\nStreamPercent=100\r\n' > "$GAMEDIR/game_capture.ini"
    echo "[deploy] wrote game_capture.ini (Color=0, StreamPercent=100)"
else
    echo "[deploy] game_capture.ini already present - left alone"
fi

# 4. An enabled ReShade technique is REQUIRED: the add-on does its per-frame work in
#    reshade_begin_effects, and ReShade only fires that when it has at least one enabled
#    effect. Without one the add-on loads but never runs, and the bridge waits forever
#    for 'Local\mgpu_mailbox_v1'. The depth-tap effect is image neutral (it writes
#    to a private target), so leaving it on costs nothing.
mkdir -p "$GAMEDIR/reshade-shaders/Shaders"
cp -f "$REPO/effects/mgpu_depth_tap.fx" "$GAMEDIR/reshade-shaders/Shaders/mgpu_depth_tap.fx" \
    && echo "[deploy] effect -> reshade-shaders/Shaders/mgpu_depth_tap.fx"
printf 'Techniques=MGPU_DepthTap@mgpu_depth_tap.fx\r\nTechniqueSorting=MGPU_DepthTap@mgpu_depth_tap.fx\r\n' \
    > "$GAMEDIR/ReShadePreset.ini"
echo "[deploy] ReShadePreset.ini enables MGPU_DepthTap"

# ReShade.ini has to look in the shader folder; ReShade rewrites this file itself, so
# only touch the one key.
if [ -f "$GAMEDIR/ReShade.ini" ]; then
    if grep -q '^EffectSearchPaths=' "$GAMEDIR/ReShade.ini"; then
        sed -i 's|^EffectSearchPaths=.*|EffectSearchPaths=.\\reshade-shaders\\Shaders\\**|' \
            "$GAMEDIR/ReShade.ini" 2>/dev/null || true
    fi
    if ! grep -q '^PresetPath=' "$GAMEDIR/ReShade.ini"; then
        sed -i '/^\[GENERAL\]/a PresetPath=.\\ReShadePreset.ini' "$GAMEDIR/ReShade.ini" 2>/dev/null || true
    fi
    echo "[deploy] ReShade.ini: EffectSearchPaths/PresetPath checked"
else
    echo "[deploy] ReShade.ini not created yet - start the game once, then re-run this"
fi

echo "[deploy] files:"
ls -la "$GAMEDIR/game_capture.addon64" "$GAMEDIR/game_capture.ini" "$GAMEDIR/dxgi.dll" 2>/dev/null | sed 's/^/[deploy]   /'
echo "[deploy] next: start the game with DLSS on, then run the bridge with --game ${EXE:-<exe>}"
