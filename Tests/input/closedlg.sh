#!/bin/bash
# Can a module dialog be closed once opened?
#
#   closedlg.sh <@index> <tag>
#
# Two halves, because neither alone answers it:
#
#   * THE BOX IS DRAWN -- a screenshot before the close, so the caption's X is
#     visible (or provably absent). WS_SYSMENU is what asks for it.
#   * THE HANDLER RUNS -- `cancel` posts the WM_COMMAND(IDCANCEL) that the box
#     posts, which is what DefDlgProc makes of WM_CLOSE for a dialog. The box
#     itself is an ImGui widget and injected presses do not activate one here.
#
# The proof of closing is the SECOND childdump: with the dialog gone,
# orbiter_ActiveDialog() no longer names it and the dump reports the render
# window's children instead of the dialog's.
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
WANT="${1:-@0}"
TAG="${2:-close}"
LOG=/tmp/cl_$TAG.txt
UISCRIPT=/tmp/cl_$TAG.ui

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

cat > "$UISCRIPT" <<EOF
wait 1200
customcmd $WANT
wait 180
log ==== OPENED ====
dump after-open
childdump
wait 900
log ==== CANCEL ====
cancel
wait 240
log ==== AFTER CANCEL ====
dump after-cancel
childdump
log ==== SWEEP END ====
EOF

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log" "$ROOT/build/imgui.ini"

setsid nohup env \
    ORBITER_UI_SCRIPT="$UISCRIPT" \
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

sleep 40
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    xdotool windowactivate --sync "$WID" 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    xdotool mousemove $((X + 500)) $((Y + 300))
fi

# Before the cancel step fires. The script holds the dialog open for 900
# frames (~15s) so this shot cannot land in the gap either side of it.
sleep 20
WID=$(xdotool search --name "rbiter" | tail -1)
[ -n "$WID" ] && import -window "$WID" "$ROOT/_shots/cl_${TAG}_open.png" 2>/dev/null
# After.
sleep 25
WID=$(xdotool search --name "rbiter" | tail -1)
[ -n "$WID" ] && import -window "$WID" "$ROOT/_shots/cl_${TAG}_closed.png" 2>/dev/null

echo "--- trace --------------------------------------------------------"
awk '/UIDRV (step|cancel|after|childdump)|====/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ------------------------------------------------------"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"
pkill -x Orbiter
