#!/bin/bash
# DX9 External MFD: does its display area draw once the MFD is POWERED ON?
#
# An ExternMFD starts powered off. IDC_BUTTON_PWR (1016) is button index 12,
# and MFDWindow::ProcessButton case 12 sends OAPI_KEY_ESCAPE -- Orbiter's MFD
# power toggle. The first sweep never pressed it, so "blank display" was not
# yet a fair reading.
#
# The button is a custom-class control handled by MFD_BtnProc, which acts on
# WM_LBUTTONDOWN/UP and never sends WM_COMMAND, so the driver's `click` cannot
# reach it. `press` can.
#
# childdump is taken FIRST, before anything is pressed, because the missing
# BUTTONS are a separate question from the missing DISPLAY and only the dump
# can tell "drew nothing" from "is not there / has no wndproc".
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
LOG=/tmp/mfdpwr.txt
UISCRIPT=/tmp/mfdpwr.ui
SHOTS=$ROOT/_shots

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

cat > "$UISCRIPT" <<'EOF'
wait 1200
log ==== OPEN ====
customcmd @3
wait 120
dump after-open
childdump
log ==== BEFORE PWR ====
wait 60
press 1016
wait 240
log ==== AFTER PWR ====
childdump
log ==== SWEEP END ====
EOF

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"

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

# Pointer inside the window before the command fires; see funcdrv.sh.
sleep 40
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    xdotool windowactivate --sync "$WID" 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    xdotool mousemove $((X + 500)) $((Y + 300))
    echo "pointer moved"
fi

# One shot before the press and one after, so the difference is the evidence.
sleep 12
mkdir -p "$SHOTS"
WID=$(xdotool search --name "rbiter" | tail -1)
[ -n "$WID" ] && import -window "$WID" "$SHOTS/mfd_before.png" 2>/dev/null
sleep 25
WID=$(xdotool search --name "rbiter" | tail -1)
[ -n "$WID" ] && import -window "$WID" "$SHOTS/mfd_after.png" 2>/dev/null

echo "--- trace --------------------------------------------------------"
awk '/UIDRV|RegisterSwap|gcCore/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ------------------------------------------------------"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"
awk '/SWEEP END/ {f=1} END {print (f ? "frames: STILL PUMPING" : "frames: STOPPED")}' "$ROOT/build/Orbiter.log"
pkill -x Orbiter
