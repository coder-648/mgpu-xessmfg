#!/usr/bin/env bash
# ===========================================================================
#  rcmd.sh - run a command the way this machine requires, then get out of the way
#
#  Why this exists: Cline's commands kept hanging because VS Code's bash shell
#  integration (`__vsc_prompt_cmd`) loses the "command finished" marker for
#  heredocs, very long compound commands, and children that keep the pty open.
#  Measured pattern (see docs/findings/TOOLING_NOTES.md):
#      short single line + redirect   -> always returns
#      heredoc                        -> always hangs
#      long compound with many quotes -> often hangs
#      cmd.exe / powershell.exe child -> often hangs
#
#  This wrapper always:
#    * detaches stdin from the pty        (</dev/null)
#    * detaches the process group          (setsid)
#    * redirects stdout+stderr to a file   (temp/cmd/<name>.log)
#    * prints one short line and exits immediately
#
#  Usage:
#    ./scripts/rcmd.sh <name> 'shell command'
#    ./scripts/rcmd.sh <name> -f path/to/script.sh      # run a script file
#  Then read temp/cmd/<name>.log (or temp/cmd/<name>.status for the exit code).
# ===========================================================================
set -uo pipefail

NAME="${1:-run}"
shift || true

if [ "${1:-}" = "-f" ]; then
    shift
    SCRIPT="${1:?script path required}"
    CMD="bash $(printf '%q' "$SCRIPT")"
else
    CMD="${1:?command required}"
fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$REPO/temp/cmd"
mkdir -p "$OUT_DIR"

LOG="$OUT_DIR/$NAME.log"
STATUS="$OUT_DIR/$NAME.status"
: > "$LOG"
echo "running" > "$STATUS"

# setsid + </dev/null so nothing keeps the pty busy; the shell returns at once
setsid bash -c "$CMD" < /dev/null > "$LOG" 2>&1 &
PID=$!

echo "[rcmd] $NAME started (pid $PID) -> temp/cmd/$NAME.log"

# Wait briefly so fast commands already have their result when Cline reads it.
for _ in $(seq 1 40); do
    if ! kill -0 "$PID" 2>/dev/null; then
        wait "$PID"
        echo "rc=$?" > "$STATUS"
        echo "[rcmd] $NAME finished rc=$(cat "$STATUS")"
        exit 0
    fi
    sleep 0.05
done

echo "[rcmd] $NAME still running (up to 2 s); check temp/cmd/$NAME.log later"
exit 0
