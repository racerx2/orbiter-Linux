#!/bin/bash
# ===========================================================================
# minimise_probe.sh -- drive the window to a ZERO-SIZED FRAMEBUFFER and back,
# and read what orbiter_PumpFrame says about it.
#
# WHAT IS UNDER TEST
#
# orbiter_PumpFrame skips the frame when glfwGetFramebufferSize reports 0x0.
# That skip used to be a bare `return`, which is the only early-out between
# the client's PresentScene and vkAcquireNextImageKHR -- and that acquire,
# with UINT64_MAX, is the one blocking call in the frame. So the skip did not
# merely drop a picture: it removed the only thing PACING the simulation, and
# said nothing at all in the log. A core taken from a session the user
# reported as frozen showed 8.6 million frames in 231 s with every GPU thread
# parked. See the porting notes and the note at the site.
#
# The skip now names itself once on entry and once on recovery, with a
# duration, and sleeps 10 ms per iteration instead of spinning. This harness
# is what establishes that the two messages actually appear, and that the
# window comes back afterwards.
#
# WHY MINIMISE
#
# A minimised (iconified) window has no framebuffer, which is the ONE
# condition that reaches the branch without a code change. xdotool's KEY and
# BUTTON synthesis does not work on this desk -- confirmed three times, see
# section F of the register -- but windowminimize/windowmap are EWMH client
# messages to the window manager, an entirely different path, so they are
# worth using here even though xdotool keys are not.
#
#   minimise_probe.sh [seconds-minimised]
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="${SCN:-Tests/Earth from 1.35 radii}"
DOWN="${1:-6}"
LOG=/tmp/minprobe.txt

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session. The
# cookie filename changes at every login, so a literal here survives exactly
# until the next reboot and then reports "no window" -- see session.sh.
. "$(dirname "$0")/session.sh"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1

# --keeplog APPENDS. Without this the run reads the PREVIOUS run's trace.
rm -f "$ROOT/build/Orbiter.log"

setsid nohup \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!

# Kill THIS run on any exit, including an interrupt -- by PID, so a session
# the user started themselves is never touched. See dlgdrv.sh for what an
# orphaned run cost once.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

sleep 45

WID=$(xdotool search --name "rbiter" | tail -1)
if [ -z "$WID" ]; then
    echo "no window -- process never got that far"
    tail -20 "$LOG"
    exit 1
fi
echo "window: $WID"
eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "geometry before: ${WIDTH}x${HEIGHT}"

echo "--- minimising for ${DOWN}s ---"
xdotool windowminimize --sync "$WID" 2>/dev/null || xdotool windowminimize "$WID"
sleep "$DOWN"

echo "--- restoring ---"
xdotool windowmap "$WID" 2>/dev/null
xdotool windowactivate "$WID" 2>/dev/null
sleep 12

eval "$(xdotool getwindowgeometry --shell "$WID")"
echo "geometry after: ${WIDTH}x${HEIGHT}"

echo "--- framebuffer messages -----------------------------------------"
awk '/framebuffer is/ {print}' "$ROOT/build/Orbiter.log"
echo "--- stuck-button messages ----------------------------------------"
awk '/was stuck/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ------------------------------------------------------"
awk '
    /framebuffer is 0x0/          { entry++ }
    /framebuffer is back at/      { back++ }
    END {
        print (entry ? "entry:    SEEN (" entry ")" : "entry:    ABSENT")
        print (back  ? "recovery: SEEN (" back  ")" : "recovery: ABSENT")
    }' "$ROOT/build/Orbiter.log"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"

pkill -x Orbiter
