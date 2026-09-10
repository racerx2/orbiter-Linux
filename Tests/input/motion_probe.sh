#!/bin/bash
# ===========================================================================
# motion_probe.sh -- is POINTER MOTION alone reaching postMouseMessages?
#
# Cut down from drag_probe.sh to isolate one question. drag_probe reported no
# [mouse] trace for a whole injected drag, and there are two very different
# explanations: the injected BUTTON is not arriving, or nothing at all is
# arriving because postMouseMessages is returning early (io.WantCaptureMouse
# with no drag in flight). No button is used here at all, so a silent run
# means the second.
#
#   motion_probe.sh [client-x] [client-y]
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="${SCN:-Tests/Earth from 1.35 radii}"
CX="${1:-300}"
CY="${2:-250}"
LOG=/tmp/motion_probe.txt

. "$(dirname "$0")/session.sh"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"

setsid nohup env ORBITER_TRACE_MSG=1 ORBITER_TRACE_MOUSE=1 \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

echo -n "waiting for the session"
for _ in $(seq 1 120); do
    if awk '/Finished initialising panels/{f=1} END{exit !f}' \
           "$ROOT/build/Orbiter.log" 2>/dev/null; then break; fi
    kill -0 "$ORBPID" 2>/dev/null || { echo; echo "FAIL: died"; exit 1; }
    echo -n "."; sleep 1
done
echo; sleep 5

WID=$(xdotool search --name "rbiter" | tail -1)
[ -z "$WID" ] && { echo "FAIL: no window"; exit 1; }
xdotool windowactivate --sync "$WID"; sleep 1
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "window $WID at ($X,$Y) ${WIDTH}x${HEIGHT}"

for i in 0 1 2 3 4 5 6 7; do
    xdotool mousemove $((X + CX + i * 25)) $((Y + CY + i * 15))
    sleep 0.4
done
sleep 2

echo "--- every [mouse] line in the run ---------------------------------"
awk '/\[mouse\]/ {print "  " $0}' "$LOG"
echo "--- last 12 lines of stderr ---------------------------------------"
tail -12 "$LOG" | sed 's/^/  /'
echo "--- still pumping? ------------------------------------------------"
A=$(awk 'END{print NR}' "$ROOT/build/Orbiter.log"); sleep 3
B=$(awk 'END{print NR}' "$ROOT/build/Orbiter.log")
echo "  Orbiter.log lines $A -> $B"
pgrep -x Orbiter >/dev/null && echo "  process: ALIVE" || echo "  process: DEAD"

xdotool mousemove 5 5
pkill -x Orbiter
