#!/bin/bash
# Controlled A/B for the BeginPaint change, one entry, imgui.ini cleared both
# times so window placement cannot drift between the runs.
#
#   dcab.sh <@index> <tag>
#
# childdump is taken as well as a screenshot: the control CLASS names say
# whether a dialog contains any self-painting custom control at all, which is
# the only kind the change can affect. A dialog of standard classes must come
# out pixel-identical.
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
WANT="${1:-@10}"
TAG="${2:-ab}"
LOG=/tmp/dcab_$TAG.txt
UISCRIPT=/tmp/dcab_$TAG.ui

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

cat > "$UISCRIPT" <<EOF
wait 1200
customcmd $WANT
wait 180
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
sleep 30

WID=$(xdotool search --name "rbiter" | tail -1)
[ -n "$WID" ] && import -window "$WID" "$ROOT/_shots/ab_$TAG.png" 2>/dev/null
awk '/UIDRV   \[|childdump:/ {print}' "$ROOT/build/Orbiter.log"
pkill -x Orbiter
