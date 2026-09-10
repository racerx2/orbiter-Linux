#!/bin/bash
# Start a session at a given fixed camera and LEAVE IT RUNNING, so a person can
# drive it by hand. One screenshot is taken on the way for the record.
#
#   live.sh "<rdist> <phi> <theta>" [tag]
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
POS="${1:-1.6 120 0}"
TAG="${2:-live}"

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
    >/tmp/live_$TAG.txt 2>&1 </dev/null & disown
ORBPID=$!
# Kill THIS run on any exit, including an interrupt -- by PID, so a
# session the user started themselves is never touched. An orphaned
# run was once found alive SIXTEEN HOURS later, sharing the GPU with
# the user's own Orbiter when that one took a SIGSEGV in the driver --
# which made the crash a contaminated sample. See
# the porting notes.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
sleep 40

WID=$(xdotool search --name "rbiter" | tail -1)
mkdir -p "$ROOT/_shots"
import -window "$WID" "$ROOT/_shots/${TAG}.png" && echo "shot: _shots/${TAG}.png"
echo "session left running; kill it with: pgrep -x Orbiter | xargs -r kill"
