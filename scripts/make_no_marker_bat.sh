#!/usr/bin/env bash
# ===========================================================================
#  Copy the marker sidecar test bat and add --no-marker, for the A/B run.
#    ./scripts/make_no_marker_bat.sh "<game exe dir>"
#  The copy is written next to the original as mgpu_test_no_marker.bat and logs
#  to bridge_no_marker.log, so the two runs can be compared line by line.
# ===========================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${1:-}"
if [ -z "$GAME" ] || [ ! -d "$GAME" ]; then
    echo "usage: $0 <game exe directory>"; exit 1
fi

python3 - "$REPO/scripts/mgpu_test_marker_sidecar.bat" "$GAME/mgpu_test_no_marker.bat" <<'PY'
import os
import sys

src, dst = sys.argv[1], sys.argv[2]
txt = open(src, "rb").read().decode("latin-1")
txt = txt.replace("set LOG=bridge_marker_sidecar.log", "set LOG=bridge_no_marker.log")
txt = txt.replace("--marker-probe --no-osd", "--marker-probe --no-marker --no-osd")
txt = txt.replace("MGPU MARKER SIDECAR TEST", "MGPU NO-MARKER A/B (decoder+repair OFF)")
open(dst, "wb").write(txt.encode("latin-1"))
print("[ab] %s (%d bytes)" % (dst, os.path.getsize(dst)))
PY
