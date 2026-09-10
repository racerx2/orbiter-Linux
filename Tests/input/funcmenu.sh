#!/bin/bash
# Exercise one entry of the toolbar's "Function" (Custom functions) menu in a
# FRESH SESSION, so an entry that kills the process cannot hide the ones after
# it.
#
#   funcmenu.sh <item-index 0..N> <tag> [close-x close-y]
#     0  = just open the Custom functions dialog and shoot it (geometry probe)
#     >0 = open the dialog, click entry <n>, shoot, then try to close it
#
# Calibration is by MEASUREMENT, not assumption: the pointer is put at a known
# screen point and the client coordinate Orbiter reports for it is read back
# out of ORBITER_TRACE_MSG. drive.sh EXITS when those disagree; here the
# difference is simply the offset, which is what a window frame is.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
ITEM="${1:-0}"
TAG="${2:-f$ITEM}"
LOG=/tmp/fm_$TAG.txt
FIFO=/tmp/uinject.fifo
UINJECT="$HERE/uinject"

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

# Toolbar button centres in CLIENT coordinates, measured off a 1272x688 shot.
FUNC_X=405
FUNC_Y=25

pgrep -x Orbiter | xargs -r kill; sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"
setsid nohup env \
    ORBITER_TRACE_MSG=1 \
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
sleep 34

WID=$(xdotool search --name "rbiter" | tail -1)
[ -z "$WID" ] && { echo "FAIL: no window"; exit 1; }
xdotool windowactivate --sync "$WID"; sleep 1
eval "$(xdotool getwindowgeometry --shell "$WID")"

mkdir -p "$ROOT/_shots"
shot() { import -window "$WID" "$ROOT/_shots/fm_${TAG}_$1.png" 2>/dev/null; echo "  shot fm_${TAG}_$1"; }
alive() { pgrep -x Orbiter >/dev/null && echo ALIVE || echo "DEAD"; }

# Injector first -- creating the uinput device warps the cursor.
rm -f "$FIFO"; mkfifo "$FIFO"
"$UINJECT" - < "$FIFO" >/dev/null 2>&1 &
UIPID=$!
exec 3>"$FIFO"
sleep 2
inj() { echo "$*" >&3; }

# --- calibrate: screen point -> client point ---------------------------------
xdotool mousemove $((X + 300)) $((Y + 250)); sleep 1.2
C=$(awk '/\[mouse\]/{l=$0} END{print l}' "$LOG" |
    sed -n 's/.*pos=(\(-\?[0-9]*\),\(-\?[0-9]*\)).*/\1 \2/p')
CX=${C% *}; CY=${C#* }
if [ -z "${CX:-}" ]; then echo "FAIL: no [mouse] trace"; exit 1; fi
OFF_X=$(( X + 300 - CX )); OFF_Y=$(( Y + 250 - CY ))
echo "window ($X,$Y) ${WIDTH}x${HEIGHT}; client origin on screen = ($OFF_X,$OFF_Y)"

cl() { xdotool mousemove $((OFF_X + $1)) $((OFF_Y + $2)); sleep 0.6; }

# --- open the Custom functions dialog ----------------------------------------
# Ctrl+F4 is OAPI_LKEY_DlgCustomCmd (Keymap.cpp:204) and lands on
# DlgMgr()->EnsureEntry<DlgFunction>() (Orbiter.cpp:2559). Clicking the
# toolbar's Function button works too, but a key needs no pixel to be right.
# The pointer is parked off the toolbar first so ImGui is not hovering it.
cl 640 400
inj "keydown LCONTROL"; sleep 0.2
inj "key F4 120";       sleep 0.2
inj "keyup LCONTROL";   sleep 1.8
shot menu
echo "  after opening menu: $(alive)"

if [ "$ITEM" != "0" ]; then
    # Entry geometry, measured from the fm_*_menu shot of the probe run.
    IT_X="${ITEM_X:-316}"
    IT_Y0="${ITEM_Y0:-187}"
    IT_DY="${ITEM_DY:-24}"
    Y=$(( IT_Y0 + (ITEM - 1) * IT_DY ))
    echo "clicking entry $ITEM at client ($IT_X,$Y)"
    cl $IT_X $Y
    inj "click l"; sleep 3.0
    shot open
    echo "  after opening entry $ITEM: $(alive)"

    if [ $# -ge 4 ]; then
        echo "closing at client ($3,$4)"
        cl "$3" "$4"
        inj "click l"; sleep 2.0
        shot closed
        echo "  after close: $(alive)"
    fi
fi

exec 3>&-; wait "$UIPID" 2>/dev/null; rm -f "$FIFO"
xdotool mousemove 5 5
echo "final: $(alive)"
pgrep -x Orbiter | xargs -r kill
