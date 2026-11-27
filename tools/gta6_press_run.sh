#!/bin/bash
# One GTA VI run with the press script that reaches the story load (see
# docs/HANDOFF.md "How far the title gets"). Usage:
#   tools/gta6_press_run.sh LOG [SECONDS] [extra xbem run options...]
# Writes the log to LOG. XBEM_RUN_DUMPS="300 330" puts guest thread dumps
# (SIGQUIT) into the log at those seconds - off by default: in runs 187/188
# the title's main loop stopped right after the dump at 330 s. Logs belong
# in ~/xbem-runs, never in the repository.
#
# The menus' timing varies between runs: a screenshot at 215 s (LOG.check.ppm)
# shows whether the story load started (black); when it did not, the run is
# stopped and started again (up to XBEM_RUN_TRIES times, default 3).
set -u
cd "$(dirname "$0")/.."
LOG=$1; SECONDS_TOTAL=${2:-430}; shift; shift || true
GAME=${XBEM_GAME_DIR:-/mnt/d/GTAVI/GTAVI}
P="A@45,A@95,A@105,A@115,A@125,A@135,A@150,DOWN@158,A@161,DOWN@168,DOWN@170,A@172,A@185,DOWN@188,A@191,DOWN@194,A@197"
# Synchronous DirectStorage queues: with the asynchronous ones ~40 % of runs
# stay on the story loading screen (a race not found yet). The title's
# watchdog crashes deliberately when its main loop stalls 30 s, which the
# emulator's heavy streaming causes: both crash instructions are skipped.
export XBEM_DSTORAGE_SYNC=${XBEM_DSTORAGE_SYNC:-1}
CHECK="$LOG.check.ppm"
# Mean brightness of a PPM (0-255); the black loading screen is ~0.
brightness() {
  python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read()
i, tok = 0, []
while len(tok) < 4:
    while d[i:i+1].isspace(): i += 1
    j = i
    while not d[j:j+1].isspace(): j += 1
    tok.append(d[i:j]); i = j
px = d[i+1:]
print(sum(px[::97]) // max(1, len(px[::97])))' "$1" 2>/dev/null || echo 255
}
for try in $(seq 1 "${XBEM_RUN_TRIES:-3}"); do
  rm -f "$CHECK"
  timeout -s INT "$SECONDS_TOTAL" ./build/bin/xbem run "$GAME" --continue-after-breakpoint --press "$P" \
    --skip-fault 0x23cf683:11 --skip-fault 0x2215c35:11 --screenshot "$CHECK@215" "$@" > "$LOG" 2>&1 &
  RUN=$!
  sleep 2
  PID=$(pgrep -f "^./build/bin/xbem run" | head -1)
  until [ -s "$CHECK" ] || ! kill -0 "$PID" 2>/dev/null; do sleep 2; done
  sleep 1
  if [ -s "$CHECK" ] && [ "$(brightness "$CHECK")" -gt 4 ]; then
    echo "try $try: still in the menus at 215 s - starting again" >&2
    kill -INT "$PID" 2>/dev/null
    wait $RUN
    continue
  fi
  for t in ${XBEM_RUN_DUMPS:-}; do
    until grep -q "running for $t s" "$LOG" 2>/dev/null; do sleep 2; kill -0 "$PID" 2>/dev/null || break 2; done
    kill -QUIT "$PID" 2>/dev/null
  done
  wait $RUN
  break
done
grep '\[status\]' "$LOG" | tail -1
