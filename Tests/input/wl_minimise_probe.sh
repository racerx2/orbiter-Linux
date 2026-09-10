#!/bin/bash
# ===========================================================================
# wl_minimise_probe.sh -- reach orbiter_PumpFrame's ZERO-FRAMEBUFFER branch on
# the platform the user actually runs, and read what it says.
#
# WHY THIS EXISTS AND minimise_probe.sh DOES NOT SUFFICE
#
# minimise_probe.sh iconifies the window with xdotool under XWayland and
# reaches NOTHING: on X11 an iconified window keeps its geometry, because
# XGetWindowAttributes answers with the stored width and height whether the
# window is mapped or not. glfwGetFramebufferSize therefore never returns 0.
#
# On WAYLAND it does. A minimised window's surface is unmapped and the
# compositor configures it to 0x0.
#
# And Wayland is what the user runs. `initialise()` in UIHost.cpp honours
# ORBITER_GLFW_PLATFORM only when it is SET; unset, GLFW prefers Wayland
# whenever a compositor is present. Every harness in this directory sets it to
# x11 -- so the whole test suite has been exercising a different backend from
# the one the user's own ./Orbiter uses. This probe is the first thing here
# that runs the user's backend on purpose.
#
# WHAT IS UNDER TEST
#
# The skip used to be a bare `return`. vkAcquireNextImageKHR -- called with
# UINT64_MAX -- is the one blocking call in the frame, so a path that returns
# before it does not drop a picture, it removes the only thing PACING the
# simulation, silently. There are five such paths and this is one of them. A
# core from a session the user reported as frozen showed 8,629,484 frames in
# 231 s with every GPU thread parked. See
# the porting notes.
#
# RESULT, 2026-09-09: it reaches the branch on NEITHER backend. KWin confirms
# minimized=true and the window keeps its framebuffer and keeps presenting.
# The probe is kept because that negative is the finding.
#
#   wl_minimise_probe.sh [seconds-minimised]
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="${SCN:-Tests/Earth from 1.35 radii}"
DOWN="${1:-8}"
LOG=/tmp/wlmin.txt
JSDIR=/tmp/orbiter_kwin

PLATFORM=wayland
export PLATFORM
. "$(dirname "$0")/session.sh"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"

QDBUS=$(command -v qdbus6 || command -v qdbus)
if [ -z "$QDBUS" ]; then
    echo "no qdbus -- cannot drive KWin"; exit 1
fi

# KWin scripts take no arguments; two concrete files are generated from the
# one template and each is loaded under its own plugin name.
mkdir -p "$JSDIR"
sed 's/@WANT@/true/'  "$(dirname "$0")/kwin_minimise.js" > "$JSDIR/min.js"
sed 's/@WANT@/false/' "$(dirname "$0")/kwin_minimise.js" > "$JSDIR/res.js"

kwin_run() {   # kwin_run <file> <plugin-name>
    "$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$2" \
        >/dev/null 2>&1
    local id
    id=$("$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
         "$1" "$2" 2>/dev/null | tail -1)
    [ -z "$id" ] && { echo "  loadScript failed for $2"; return 1; }
    "$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
    "$QDBUS" org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run \
        >/dev/null 2>&1
    sleep 1
    "$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$2" \
        >/dev/null 2>&1
    return 0
}

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1

# --keeplog APPENDS. Without this the run reads the PREVIOUS run's trace.
rm -f "$ROOT/build/Orbiter.log"

SINCE=$(date '+%Y-%m-%d %H:%M:%S')

setsid nohup \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!

# Kill THIS run on any exit, including an interrupt -- by PID, so a session the
# user started themselves is never touched. See dlgdrv.sh for what an orphaned
# run cost once.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

sleep 45
if ! kill -0 "$ORBPID" 2>/dev/null; then
    echo "process gone before the test began:"; tail -25 "$LOG"; exit 1
fi
echo "backend: $(awk '/backend|platform/ {print; exit}' "$LOG")"

echo "--- minimising for ${DOWN}s ---"
kwin_run "$JSDIR/min.js" orbiterprobemin
sleep "$DOWN"

echo "--- restoring ---"
kwin_run "$JSDIR/res.js" orbiterproberes
sleep 12

echo "--- what KWin saw -------------------------------------------------"
journalctl --user --since "$SINCE" --no-pager 2>/dev/null \
    | awk '/orbiterprobe:/ {sub(/^.*orbiterprobe: /, "  "); print}'

echo "--- framebuffer messages -----------------------------------------"
awk '/framebuffer is/ {print}' "$ROOT/build/Orbiter.log"
echo "--- stuck-button messages ----------------------------------------"
awk '/was stuck/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ------------------------------------------------------"
awk '
    /framebuffer is 0x0/     { entry++ }
    /framebuffer is back at/ { back++ }
    END {
        print (entry ? "entry:    SEEN (" entry ")" : "entry:    ABSENT")
        print (back  ? "recovery: SEEN (" back  ")" : "recovery: ABSENT")
    }' "$ROOT/build/Orbiter.log"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"

pkill -x Orbiter
