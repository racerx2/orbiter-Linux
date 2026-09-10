#!/bin/bash
# Step the external camera out one notch at a time and shoot every step, so a
# change that happens BETWEEN two notches can be found by differencing rather
# than by eye.
#
# PageUp is TrackCamRetreat (Keymap.cpp: OAPI_KEY_PRIOR). No mouse is used and
# no calibration is needed -- keys go to the focused window -- which is why
# this does not source drive.sh.
#
#   zoomstep.sh "<rdist> <phi> <theta>" <tag> [steps]
# writes _shots/zs_<tag>_NN.png
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
POS="${1:-2.49 61.5 -4.9}"
TAG="${2:-a}"
STEPS="${3:-12}"
FIFO=/tmp/uinject.fifo
UINJECT="$HERE/uinject"

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

SCNFILE="$ROOT/build/Scenarios/$SCN.scn"
cp "$ROOT/Scenarios/$SCN.scn" "$SCNFILE"
sed -i "s/^  POS .*/  POS $POS/" "$SCNFILE"
echo "camera POS $POS"

pgrep -x Orbiter | xargs -r kill; sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"
setsid nohup \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >/tmp/zs_$TAG.txt 2>&1 </dev/null & disown
ORBPID=$!
# Kill THIS run on any exit, including an interrupt -- by PID, so a
# session the user started themselves is never touched. An orphaned
# run was once found alive SIXTEEN HOURS later, sharing the GPU with
# the user's own Orbiter when that one took a SIGSEGV in the driver --
# which made the crash a contaminated sample. See
# the porting notes.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
sleep 38

WID=$(xdotool search --name "rbiter" | tail -1)
[ -z "$WID" ] && { echo "FAIL: no window"; exit 1; }
xdotool windowactivate --sync "$WID"; sleep 1

rm -f "$FIFO"; mkfifo "$FIFO"
"$UINJECT" - < "$FIFO" >/dev/null 2>&1 &
UIPID=$!
exec 3>"$FIFO"
sleep 2

mkdir -p "$ROOT/_shots"
shot() { import -window "$WID" "$ROOT/_shots/zs_${TAG}_$1.png" 2>/dev/null; echo "  shot zs_${TAG}_$1"; }

printf -v idx "%02d" 0
shot "$idx"
n=0
while [ $n -lt "$STEPS" ]; do
    n=$((n+1))
    echo "key PRIOR 120" >&3      # one TrackCamRetreat notch
    sleep 1.2
    printf -v idx "%02d" "$n"
    shot "$idx"
done

exec 3>&-; wait "$UIPID" 2>/dev/null; rm -f "$FIFO"
xdotool mousemove 5 5
pgrep -x Orbiter | xargs -r kill
