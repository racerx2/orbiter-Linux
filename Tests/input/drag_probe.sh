#!/bin/bash
# ===========================================================================
# drag_probe.sh -- does a HELD button survive a drag?
#
# WHAT IS UNDER TEST, AND WHY IT IS A RISK
#
# orbiter_PumpFrame now corrects a stuck mouse button after glfwPollEvents:
#
#     if (mio.MouseDown[b] && glfwGetMouseButton(g_window, b) == GLFW_RELEASE)
#         mio.MouseDown[b] = false;
#
# That exists because a core taken from a session the user reported as frozen
# had IO.MouseDown[0] true with nobody touching the mouse -- a release lost to
# a compositor move grab. See the porting notes.
#
# The correction writes ImGui's button state DIRECTLY. If GLFW's polled level
# state ever lagged the event queue, this would cancel legitimate presses and
# every drag in the program -- a dialog title bar, a trackbar, camera
# rotation -- would break. The argument that it cannot is at the site; this is
# the measurement.
#
# METHOD: press in the RENDER area (not the menu bar -- postMouseMessages
# correctly declines to forward while WantCaptureMouse is set, so a menu-bar
# click produces no btn= trace and proves nothing), then move in steps with
# the button held, then release. ORBITER_TRACE_MSG=1 prints one [mouse] line
# per forwarded message; the run passes if btn=1 is carried through EVERY move
# and btn=0 returns after the release.
#
#   drag_probe.sh [client-x] [client-y] [steps]
# ===========================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/racerx/orbiter-native
SCN="${SCN:-Tests/Earth from 1.35 radii}"
CX="${1:-300}"
CY="${2:-250}"
STEPS="${3:-8}"
LOG=/tmp/drag_probe.txt
FIFO=/tmp/uinject_drag.fifo
UINJECT="$HERE/uinject"

. "$(dirname "$0")/session.sh"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"

setsid nohup env ORBITER_TRACE_MSG=1 ORBITER_TRACE_MOUSE=1 \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null; rm -f "$FIFO"' \
    EXIT INT TERM

# WAIT FOR THE SESSION, do not guess at it.
#
# postMouseMessages' first line is `if (!g_sessionActive) return;`, so until
# clbkPostCreation has run there is no [mouse] trace no matter what is
# injected -- and a fixed sleep that lands one second early reports "the drag
# was not delivered" on a working build. The scene's own first log line is the
# signal. 120 s ceiling: a cold texture cache can take over a minute.
echo -n "waiting for the session"
for _ in $(seq 1 120); do
    if awk '/Finished initialising panels/{f=1} END{exit !f}' \
           "$ROOT/build/Orbiter.log" 2>/dev/null; then break; fi
    if ! kill -0 "$ORBPID" 2>/dev/null; then
        echo; echo "FAIL: process died during load"; tail -20 "$LOG"; exit 1
    fi
    echo -n "."; sleep 1
done
echo
sleep 4

WID=$(xdotool search --name "rbiter" | tail -1)
[ -z "$WID" ] && { echo "FAIL: no window"; tail -20 "$LOG"; exit 1; }
xdotool windowactivate --sync "$WID"; sleep 1
echo "focused window: $(xdotool getactivewindow 2>/dev/null) (want $WID)"
eval "$(xdotool getwindowgeometry --shell "$WID")"
WX=$X; WY=$Y

# Client origin, measured rather than assumed: move somewhere known and read
# back what the app says the client position was. Same technique as
# clicktest.sh, and for the same reason -- the frame offset is not knowable
# from the geometry alone.
xdotool mousemove $((WX + CX)) $((WY + CY)); sleep 1.2
C=$(awk '/\[mouse\]/{l=$0} END{print l}' "$LOG" |
    sed -n 's/.*pos=(\(-\?[0-9]*\),\(-\?[0-9]*\)).*/\1 \2/p')
RX=${C% *}; RY=${C#* }
[ -z "${RX:-}" ] && { echo "FAIL: no [mouse] trace at all"; exit 1; }
OFF_X=$(( WX + CX - RX )); OFF_Y=$(( WY + CY - RY ))
echo "client origin = ($OFF_X,$OFF_Y); starting drag at client ($CX,$CY)"

rm -f "$FIFO"; mkfifo "$FIFO"
# STDERR KEPT. uinject reports a failed /dev/uinput open, a failed
# UI_DEV_CREATE and every rejected write on stderr; discarding it turns "the
# virtual device was never created" into "the drag was not delivered", which
# reads as an application defect.
"$UINJECT" - < "$FIFO" > /tmp/uinject_drag.log 2>&1 &
UIPID=$!
exec 3>"$FIFO"
sleep 2
inj() { echo "$*" >&3; }

# The [mouse] trace is written by ORBITER_TRACE_MSG to STDERR, which is $LOG.
# Orbiter.log is oapiWriteLog's file and carries the stuck-button line -- two
# different destinations, and reading the wrong one reports an empty trace on
# a working drag.
MARK=$(awk 'END{print NR}' "$LOG")

xdotool mousemove $((OFF_X + CX)) $((OFF_Y + CY)); sleep 0.6
inj "btn l down"; sleep 0.5
for i in $(seq 1 "$STEPS"); do
    xdotool mousemove $((OFF_X + CX + i * 12)) $((OFF_Y + CY + i * 6))
    sleep 0.25
done
inj "btn l up"; sleep 1.5
xdotool mousemove $((OFF_X + CX + STEPS * 12 + 40)) $((OFF_Y + CY))
sleep 1.0

exec 3>&-; wait "$UIPID" 2>/dev/null; rm -f "$FIFO"

echo "--- uinject --------------------------------------------------------"
if [ -s /tmp/uinject_drag.log ]; then sed 's/^/  /' /tmp/uinject_drag.log
else echo "  (silent -- device created and every event written)"; fi
echo "--- [btn] GLFW's view vs ImGui's ----------------------------------"
awk '/\[btn\]/ {print "  " $0}' "$LOG"
echo "--- [mouse] trace from the drag -----------------------------------"
awk -v m="$MARK" 'FNR>m && /\[mouse\]/ {print "  " $0}' "$LOG"
echo "--- stuck-button corrections --------------------------------------"
awk '/was stuck/ {print "  " $0}' "$ROOT/build/Orbiter.log"
echo "--- verdict -------------------------------------------------------"
# THE VERDICT IS READ FROM [btn], NOT FROM [mouse], and the difference is the
# whole point of this harness.
#
# [mouse] is what postMouseMessages forwarded, and that function returns early
# whenever io.WantCaptureMouse is set with no drag in flight -- correctly, it
# is this port's translation of "the pointer is over another window". So an
# empty [mouse] trace means "the pointer was over a dialog", not "the button
# did not work", and reading a button verdict out of it produces a confident
# FALSE NEGATIVE. That is exactly what the first two runs of this probe did.
#
# [btn] is unconditional and reports both layers: glfw= is the platform's own
# LEVEL state, imgui= is what the widget layer believes. The press must appear
# in both, and the release must clear both.
awk '
    /\[btn\]/ {
        g = m = -1
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^glfw=/)  { sub(/glfw=/,  "", $i); g = $i + 0 }
            if ($i ~ /^imgui=/) { sub(/imgui=/, "", $i); m = $i + 0 }
        }
        if (g == 1)            glfwsaw = 1
        if (g == 1 && m == 1)  both    = 1
        if (both && g == 0 && m == 0) cleared = 1
    }
    END {
        if (!glfwsaw) {
            print "  PRESS:   GLFW never saw the button -- the event did not"
            print "           reach the process. Nothing about the port is"
            print "           being tested here; fix the injection first."
        } else if (!both) {
            print "  PRESS:   GLFW saw it, ImGui did NOT -- the press was lost"
            print "           between the platform and the widget layer. FAIL"
        } else {
            print "  PRESS:   reached GLFW and then ImGui -- PASS"
        }
        if (both)
            print (cleared ? "  RELEASE: cleared both layers -- PASS"
                           : "  RELEASE: never cleared -- the button is STUCK. FAIL")
    }' "$LOG"

xdotool mousemove 5 5
pkill -x Orbiter
