#!/bin/bash
# Does an injected click reach the ImGui toolbar at all?
#
# Clicks one toolbar button by CLIENT coordinate and shoots before and after.
# "Options" is the control: it opens a dialog, so the answer is visible.
#
#   clicktest.sh <client-x> <client-y> <tag>
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
CX_WANT="${1:-511}"
CY_WANT="${2:-25}"
TAG="${3:-a}"
LOG=/tmp/ct_$TAG.txt
FIFO=/tmp/uinject.fifo
UINJECT="$HERE/uinject"

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

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
WX=$X; WY=$Y

mkdir -p "$ROOT/_shots"
shot() { import -window "$WID" "$ROOT/_shots/ct_${TAG}_$1.png" 2>/dev/null; echo "  shot ct_${TAG}_$1"; }

rm -f "$FIFO"; mkfifo "$FIFO"
"$UINJECT" - < "$FIFO" >/dev/null 2>&1 &
UIPID=$!
exec 3>"$FIFO"
sleep 2
inj() { echo "$*" >&3; }

xdotool mousemove $((WX + 300)) $((WY + 250)); sleep 1.2
C=$(awk '/\[mouse\]/{l=$0} END{print l}' "$LOG" |
    sed -n 's/.*pos=(\(-\?[0-9]*\),\(-\?[0-9]*\)).*/\1 \2/p')
CX=${C% *}; CY=${C#* }
[ -z "${CX:-}" ] && { echo "FAIL: no [mouse] trace"; exit 1; }
OFF_X=$(( WX + 300 - CX )); OFF_Y=$(( WY + 250 - CY ))
echo "window ($WX,$WY) ${WIDTH}x${HEIGHT}  client origin = ($OFF_X,$OFF_Y)"

shot before
echo "moving to client ($CX_WANT,$CY_WANT) = screen ($((OFF_X+CX_WANT)),$((OFF_Y+CY_WANT)))"
xdotool mousemove $((OFF_X + CX_WANT)) $((OFF_Y + CY_WANT)); sleep 1.2
echo -n "  pointer now: "; xdotool getmouselocation
shot hover
# HELD, not a `click`. ImGui only sees a button that is down when NewFrame
# samples it; at 150 fps a press-and-release a few milliseconds long can fall
# between two frames entirely, and the button never activates. The hover
# highlight proves the pointer is being seen -- it is the press that was
# too short.
if [ "${USE_XDO:-0}" = "1" ]; then
    xdotool click 1; sleep 2.0
else
    inj "btn l down"; sleep 0.35
    inj "btn l up";   sleep 2.0
fi
shot after
echo "  last mouse trace: $(awk '/\[mouse\]/{l=$0} END{print l}' "$LOG")"
echo "  button traces:"; awk '/btn=[1-9]/ {print "    "$0}' "$LOG" | tail -4

exec 3>&-; wait "$UIPID" 2>/dev/null; rm -f "$FIFO"
xdotool mousemove 5 5
pgrep -x Orbiter | xargs -r kill
