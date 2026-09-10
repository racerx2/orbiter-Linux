#!/bin/bash
# Does a Playback scenario actually play back?
#
# "Playback/Glider in orbit 1" is driven by Flights/Glider in orbit 1/, whose
# system.dat cuts the camera to the cockpit at recording time 20.87 s. The
# scenario itself opens in an external tracking view (BEGIN_CAMERA MODE
# Extern), so the view at T+~35 s of simulated time is a one-bit answer:
#
#   external view  -> the recording was never opened
#   cockpit view   -> playback is running and the system-event stream is read
#
#   playback_probe.sh <tag> [wait-seconds] [scenario]
# writes _shots/playback_<tag>.png
set -u
TAG="${1:-run}"
WAIT="${2:-70}"
SCN="${3:-Playback/Glider in orbit 1}"
ROOT=/home/racerx/orbiter-native

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

pgrep -x Orbiter | xargs -r kill; sleep 2

cd "$ROOT/build" || exit 1
setsid nohup ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >/tmp/playback_$TAG.txt 2>&1 </dev/null &
disown
ORBPID=$!
# Kill THIS run on any exit, including an interrupt -- by PID, so a
# session the user started themselves is never touched. An orphaned
# run was once found alive SIXTEEN HOURS later, sharing the GPU with
# the user's own Orbiter when that one took a SIGSEGV in the driver --
# which made the crash a contaminated sample. See
# the porting notes.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
sleep "$WAIT"

if ! pgrep -x Orbiter >/dev/null; then
    echo "NOT RUNNING -- see /tmp/playback_$TAG.txt"; exit 1
fi

WID=$(xdotool search --name "Orbiter" | tail -1)
mkdir -p "$ROOT/_shots"
import -window "$WID" "$ROOT/_shots/playback_$TAG.png" && \
    echo "shot: _shots/playback_$TAG.png"

pgrep -x Orbiter | xargs -r kill
