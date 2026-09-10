#!/bin/bash
# Does a BARE RIGHT-CLICK -- press and release with the hand perfectly still --
# move the camera?
#
# On Windows it must not. Orbiter::InitRotationMode does ShowCursor(FALSE) +
# SetCapture + ClipCursor, and not one of those moves the pointer, so the first
# Camera::UpdateMouse after the press reads the same position the
# WM_RBUTTONDOWN carried and dx/dy come out zero.
#
# The pointer is deliberately parked AWAY FROM THE WINDOW CENTRE, because the
# thing being tested is whether anything recentres it: a probe that clicks at
# the centre would measure a zero jump even if it did.
#
#   rclick_probe.sh [tag] [client-x] [client-y]
# writes _shots/rc_<tag>_{before,down,up}.png and prints the trace.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
TAG="${1:-a}"
WANT_X="${2:-300}"
WANT_Y="${3:-200}"
LOG=/tmp/rclick_$TAG.txt
FIFO=/tmp/uinject.fifo
UINJECT="$HERE/uinject"

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

# Put the camera somewhere with a recognisable horizon so a flip is obvious.
SCNFILE="$ROOT/build/Scenarios/$SCN.scn"
cp "$ROOT/Scenarios/$SCN.scn" "$SCNFILE"
sed -i "s/^  POS .*/  POS 1.6 120 0/" "$SCNFILE"

pgrep -x Orbiter | xargs -r kill; sleep 3
cd "$ROOT/build" || exit 1
# --keeplog APPENDS. Without this the trace read afterwards is the previous
# run's, which is a very convincing way to measure the wrong thing.
rm -f "$ROOT/build/Orbiter.log"
setsid nohup env \
    ORBITER_TRACE_MOUSE=1 ORBITER_TRACE_MSG=1 \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!
# Kill THIS run on any exit, including an interrupt -- by PID, so a
# session the user started themselves is never touched. An orphaned
# run was once found alive SIXTEEN HOURS later, sharing the GPU with
# the user's own Orbiter when that one took a SIGSEGV in the driver --
# which made the crash a contaminated sample. See
# the porting notes.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
sleep 38

WID=$(xdotool search --name "OpenOrbiter" | tail -1)
[ -z "$WID" ] && { echo "FAIL: no window"; exit 1; }
xdotool windowactivate --sync "$WID"; sleep 1
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "window at ($X,$Y) size ${WIDTH}x${HEIGHT}; centre would be $((WIDTH/2)),$((HEIGHT/2))"

mkdir -p "$ROOT/_shots"
shot() { import -window "$WID" "$ROOT/_shots/rc_${TAG}_$1.png" 2>/dev/null; echo "  shot rc_${TAG}_$1"; }

# The injector is opened FIRST: creating a uinput device warps the cursor, so
# anything positioned before it would be moved out from under the test.
rm -f "$FIFO"; mkfifo "$FIFO"
"$UINJECT" - < "$FIFO" >/dev/null 2>&1 &
UIPID=$!
exec 3>"$FIFO"
sleep 2
inj() { echo "$*" >&3; }

xdotool mousemove $((X + WANT_X)) $((Y + WANT_Y)); sleep 1.5
shot before

inj "btn r down"; sleep 2.0
shot down

inj "btn r up";   sleep 1.5
shot up

exec 3>&-; wait "$UIPID" 2>/dev/null; rm -f "$FIFO"

# KEEP=1 leaves the session up so a person can drive it by hand afterwards.
# The pointer is NOT parked in that case -- it is theirs again.
if [ "${KEEP:-0}" = "1" ]; then
    echo "session left running (KEEP=1); kill it with: pgrep -x Orbiter | xargs -r kill"
else
    xdotool mousemove 5 5
    pgrep -x Orbiter | xargs -r kill
    sleep 1
fi

echo "=== UpdateMouse trace ==="
awk '/UpdateMouse/ {print}' "$ROOT/build/Orbiter.log" | head -40
