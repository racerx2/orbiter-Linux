#!/bin/bash
# Launch Orbiter once on this desk, wait for it to settle, report.
#   run_once.sh "<scenario>" [seconds]
# Environment matches what the other harnesses in this directory use.
SCN="${1:-Delta-glider/Cape Canaveral}"
WAIT="${2:-42}"
cd /home/racerx/orbiter-native/build || exit 1

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

setsid nohup ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >/tmp/orbiter_stdout.txt 2>&1 </dev/null &
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

if pgrep -x Orbiter >/dev/null; then echo "RUNNING"; else echo "NOT RUNNING"; fi
