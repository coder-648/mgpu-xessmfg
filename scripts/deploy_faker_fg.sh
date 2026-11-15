#!/usr/bin/env bash
# ===========================================================================
#  Deploy fakerFG into one game directory (CP2077 is the reference title).
#
#    ./scripts/deploy_faker_fg.sh "<game exe dir>"          e.g.
#    ./scripts/deploy_faker_fg.sh "/mnt/e/.../Cyberpunk 2077/bin/x64"
#    ./scripts/deploy_faker_fg.sh --dry-run <dir>
#    ./scripts/deploy_faker_fg.sh --uninstall <dir>
#
#  What goes where, and why the names are what they are:
#
#    fakerFG.dll  -> dxgi.dll          we must be loaded before slInit, and the
#                                      factory entry is where the swap chain is
#    fakerFG.dll  -> nvngx_dlssg.dll   we are the DLSS-G module the plugin caches
#    nvngx_dlssg.dll -> nvngx_dlssg_real.dll   the title's own runtime (observe/pass-through)
#    ReShade      -> ReShade64.dll     fakerFG loads it (InjectReshade=1)
#    fakerFG.ini                       fakerFG's own config (created once)
#
#  dlssg_sm86 must NOT be present at the same time: it replaces nvngx_dlssg.dll
#  as well, and two providers cannot both own that name. fakerFG answers the
#  architecture question itself (ArchSpoof=1), so nothing is lost.
# ===========================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="install"
case "${1:-}" in
    --dry-run)   MODE="dry";   shift ;;
    --uninstall) MODE="remove"; shift ;;
esac
GAME="${1:-}"
if [ -z "$GAME" ] || [ ! -d "$GAME" ]; then
    echo "usage: $0 [--dry-run|--uninstall] <game exe directory>"; exit 1
fi

FG="$REPO/build/fakerFG.dll"
PRODUCER="$REPO/build/xess_mfg_producer.dll"
FG_INI="$REPO/packaging/reshade_pack/fakerFG.ini"

say() { echo "[fakerFG] $*"; }
act() {
    if [ "$MODE" = "dry" ]; then say "would: $*"; else say "$*"; eval "$*"; fi
}

say "target: $GAME"

if [ "$MODE" = "remove" ]; then
    # Restore in reverse order; a missing backup is named, never guessed at.
    for pair in "dxgi.dll:fakerFG.dll.bak" "nvngx_dlssg.dll:nvngx_dlssg.dll.bak" \
                "ReShade64.dll:ReShade64.dll.bak"; do
        live="${pair%%:*}"; bak="${pair##*:}"
        if [ -f "$GAME/$bak" ]; then
            act "mv -f \"$GAME/$live\" \"$GAME/$live.fakerFG_off\" 2>/dev/null || true"
            act "mv -f \"$GAME/$bak\" \"$GAME/$live\""
        fi
    done
    if [ -f "$GAME/nvngx_dlssg_real.dll" ]; then
        act "mv -f \"$GAME/nvngx_dlssg_real.dll\" \"$GAME/nvngx_dlssg.dll\""
    fi
    say "done. fakerFG files were moved aside (*.fakerFG_off); delete them when satisfied."
    exit 0
fi

[ -f "$FG" ] || { echo "[fakerFG] no $FG - run ./scripts/build_faker_fg.sh"; exit 1; }

# --- 1. the real DLSS-G runtime: rename once, never twice ---------------------
if [ -f "$GAME/nvngx_dlssg.dll" ] && [ ! -f "$GAME/nvngx_dlssg_real.dll" ]; then
    if grep -qi 'MgpuFgGetApi' "$GAME/nvngx_dlssg.dll" 2>/dev/null; then
        say "nvngx_dlssg.dll is already fakerFG and no _real copy exists -"
        say "  put the title's original DLSS-G module back by hand before deploying."
    else
        act "mv -f \"$GAME/nvngx_dlssg.dll\" \"$GAME/nvngx_dlssg_real.dll\""
    fi
elif [ -f "$GAME/nvngx_dlssg_real.dll" ]; then
    say "nvngx_dlssg_real.dll present - the title's runtime is already displaced"
fi

# --- 2. ReShade (optional since 2026-11-14) ---------------------------------
# The producer replaced the ReShade add-on, so ReShade is only needed if the user
# wants other shaders. fakerFG still loads it when InjectReshade=1.
if [ -f "$GAME/ReShade64.dll" ]; then
    say "ReShade64.dll present (optional - only used when InjectReshade=1)"
elif [ -f "$GAME/dxgi.dll" ] && grep -qi 'reshade' "$GAME/dxgi.dll" 2>/dev/null; then
    act "mv -f \"$GAME/dxgi.dll\" \"$GAME/ReShade64.dll\""
else
    say "no ReShade found - fine: the transport no longer needs it"
fi

# --- 3. us: dxgi.dll + nvngx_dlssg.dll (same file, two names) ---------------
if [ -f "$GAME/dxgi.dll" ] && ! grep -qi 'MgpuFgGetApi' "$GAME/dxgi.dll" 2>/dev/null; then
    act "mv -f \"$GAME/dxgi.dll\" \"$GAME/dxgi.dll.bak\""
fi
act "cp -f \"$FG\" \"$GAME/dxgi.dll\""
act "cp -f \"$FG\" \"$GAME/nvngx_dlssg.dll\""

# --- 4. the transport (xess_mfg_producer.dll) -------------------------------
# Loaded by fakerFG on its loader thread. Absent = FG chain still works, no data.
if [ -f "$PRODUCER" ]; then
    act "cp -f \"$PRODUCER\" \"$GAME/xess_mfg_producer.dll\""
else
    say "note: build/xess_mfg_producer.dll absent - run ./scripts/build_producer.sh"
fi
# The ReShade add-on is retired: remove a stale copy so it cannot run alongside the
# producer (two transports would fight over the mapping).
if [ -f "$GAME/game_capture.addon64" ]; then
    act "mv -f \"$GAME/game_capture.addon64\" \"$GAME/game_capture.addon64.retired\""
fi

# --- 5. fakerFG.ini: created once -------------------------------------------
if [ -f "$GAME/fakerFG.ini" ]; then
    say "fakerFG.ini exists - left untouched (delete it for the template)"
elif [ -f "$FG_INI" ]; then
    act "cp -f \"$FG_INI\" \"$GAME/fakerFG.ini\""
fi

# --- 6. things that must NOT be there --------------------------------------
if [ -f "$GAME/version.dll" ]; then
    say "WARNING: version.dll present (dlssg_sm86). It replaces nvngx_dlssg.dll too."
    say "         Remove dlssg_sm86/ and version.dll for this test; fakerFG answers the"
    say "         architecture question itself."
fi

say "done. Start the game and watch <exe>_fakerFG.log and <exe>_mfg_producer.log:"
say "      'loaded as dxgi.dll', 'arch: answering', 'xefg: swap chain is ours now',"
say "      'record: upscaler lane (tagged, post-MvResample)', 'producer: ... -> start=0',"
say "      'shared ready: ...', 'publish #1 ...', and the bridge receiving api=fakerFG"
