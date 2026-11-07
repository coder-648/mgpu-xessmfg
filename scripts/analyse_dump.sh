#!/usr/bin/env bash
# Wrapper for scripts/analyse_dump.py: picks a python that actually has numpy.
#
#   ./scripts/analyse_dump.sh <DUMP_DIR>
#     e.g. ./scripts/analyse_dump.sh /mnt/c/mgpu/dump           (WSL)
#          analyse_dump.sh <STAGE_DIR>/dump                   (Windows python)
#   <DUMP_DIR> is whatever you passed to bridge.exe --dump-dir.
#
# miniconda's base env has numpy on this machine; the system python3 does not.
set -uo pipefail

CANDIDATES=(
    "$HOME/miniconda3/bin/python"
    "$HOME/anaconda3/bin/python"
    "/opt/conda/bin/python"
    "python3"
)

for py in "${CANDIDATES[@]}"; do
    if [ -x "$py" ] || command -v "$py" >/dev/null 2>&1; then
        if "$py" -c 'import numpy' >/dev/null 2>&1; then
            exec "$py" "$(dirname "${BASH_SOURCE[0]}")/analyse_dump.py" "$@"
        fi
    fi
done

echo "[analyse] no python with numpy found (try: ~/miniconda3/bin/python)" >&2
exit 1
