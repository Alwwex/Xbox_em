#!/bin/bash
# One GTA VI run with the press script that reaches the story load (see
# docs/HANDOFF.md "How far the title gets"). Usage:
#   tools/gta6_press_run.sh LOG [SECONDS] [extra xbem run options...]
# Writes the log to LOG; at 300 s and 330 s a guest thread dump (SIGQUIT)
# lands in the log. Logs belong in ~/xbem-runs, never in the repository.
set -u
cd "$(dirname "$0")/.."
LOG=$1; SECONDS_TOTAL=${2:-430}; shift; shift || true
GAME=${XBEM_GAME_DIR:-/mnt/d/GTAVI/GTAVI}
P="A@45,A@95,A@105,A@115,A@125,A@135,A@150,DOWN@158,A@161,DOWN@168,DOWN@170,A@172,A@185,DOWN@188,A@191,DOWN@194,A@197"
timeout -s INT "$SECONDS_TOTAL" ./build/bin/xbem run "$GAME" --continue-after-breakpoint --press "$P" "$@" > "$LOG" 2>&1 &
RUN=$!
sleep 2
PID=$(pgrep -f "^./build/bin/xbem run" | head -1)
for t in 300 330; do
  until grep -q "running for $t s" "$LOG" 2>/dev/null; do sleep 2; kill -0 "$PID" 2>/dev/null || break 2; done
  kill -QUIT "$PID" 2>/dev/null
done
wait $RUN
grep '\[status\]' "$LOG" | tail -1
