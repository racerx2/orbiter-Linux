#!/bin/bash
# Drive the LAUNCHPAD rather than a session.
#
#   launchpad.sh <tag> [ui-script-extra-lines-file]
#
# Every other harness here passes --scenariox and jumps straight into a
# scenario, so the Launchpad -- which is most of the Win32 control surface in
# the program, and the only place several style bits are used at all -- had
# never been screenshotted by a test.
set -u
ROOT=/home/racerx/orbiter-native
TAG="${1:-lp}"
EXTRA="${2:-}"
LOG=/tmp/lp_$TAG.txt
UISCRIPT=/tmp/lp_$TAG.ui

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

{
  echo "wait 60"
  echo "log ==== LAUNCHPAD ===="
  echo "dump lp"
  echo "childdump"
  [ -n "$EXTRA" ] && cat "$EXTRA"
  echo "log ==== END ===="
} > "$UISCRIPT"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"

# NO --scenariox: stop at the Launchpad.
setsid nohup env \
    ORBITER_UI_SCRIPT="$UISCRIPT" \
    ./Orbiter --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!
# Kill THIS run on any exit, including an interrupt -- by PID, so a
# session the user started themselves is never touched. An orphaned
# run was once found alive SIXTEEN HOURS later, sharing the GPU with
# the user's own Orbiter when that one took a SIGSEGV in the driver --
# which made the crash a contaminated sample. See
# the porting notes.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

sleep 22
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    xdotool windowactivate --sync "$WID" 2>/dev/null
    import -window "$WID" "$ROOT/_shots/lp_$TAG.png" 2>/dev/null
    echo "shot: _shots/lp_$TAG.png"
else
    echo "no window"
fi

echo "--- trace --------------------------------------------------------"
awk '/UIDRV/ {print}' "$ROOT/build/Orbiter.log" | head -60
pkill -x Orbiter
