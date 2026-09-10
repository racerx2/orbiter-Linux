#!/bin/bash
# ===========================================================================
# dlgdrv.sh -- open one custom-function dialog and run an arbitrary driver
# fragment against it.
#
# funcdrv.sh writes its own fixed script and can only run the command; this
# takes the fragment as a file, so a dialog can be opened AND then driven
# (click a button, dump the children, select a tree row) in the same session.
#
#   dlgdrv.sh "<custom command substring>" <fragment file> [tag]
#
# WINDOW SIZE MATTERS for the wide dialogs. Vulkan Debug Controls is 400
# dialog units wide and its second column -- IDC_DBG_RESBIAS among others --
# falls outside a 1272x688 client area entirely, which reads as "the control
# is missing" rather than "the window is too small". build/Orbiter.cfg's
# WindowWidth/WindowHeight is the lever.
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
# SCN= in the environment picks the scenario. The default is the single-vessel
# test case; anything about lists or ordering needs a populated one, e.g.
# SCN="Delta-glider/Docked at ISS" (ISS, Luna-OB1, GL-01, GL-02, SH-01).
SCN="${SCN:-Tests/Earth from 1.35 radii}"
CMD="${1:?usage: dlgdrv.sh <command substring> <fragment> [tag]}"
FRAG="${2:?usage: dlgdrv.sh <command substring> <fragment> [tag]}"
TAG="${3:-dlg}"
LOG=/tmp/dd_$TAG.txt
UISCRIPT=/tmp/dd_$TAG.ui
SHOTS=$ROOT/_shots

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"
LOAD_FRAMES="${LOAD_FRAMES:-900}"

# CMD "-" means "open nothing, just run the fragment" -- for the core dialogs,
# which come off the MENU BAR (menucmd) rather than the custom-function list.
{
    echo "wait $LOAD_FRAMES"
    echo "log ==== BEGIN ===="
    if [ "$CMD" != "-" ]; then
        echo "customcmd $CMD"
        echo "wait 120"
    fi
    cat "$FRAG"
    echo "log ==== END ===="
} > "$UISCRIPT"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1

# --keeplog APPENDS. Without this the run reads the PREVIOUS run's trace.
rm -f "$ROOT/build/Orbiter.log"

setsid nohup env \
    ORBITER_UI_SCRIPT="$UISCRIPT" \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!

# KILL THIS RUN'S SESSION ON ANY EXIT, including an interrupt.
#
# The harnesses pkill at the START of a run and not at the end, so a run that
# is interrupted -- which is most of them, while a test is being developed --
# leaves Orbiter alive indefinitely. One was found still running SIXTEEN HOURS
# later, and it was alive when the user's own ./Orbiter took a SIGSEGV inside
# the GPU driver: two instances on one GPU, which makes that crash a
# contaminated sample. See the porting notes.
#
# By PID rather than `pkill -x Orbiter`, so this never kills a session the
# user started themselves alongside a test.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

# The pointer must be in the window before a dialog that positions itself at
# ImGui::GetMousePos() opens; see funcdrv.sh.
sleep 40
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ] && [ -n "${POINTER:-}" ]; then
    xdotool windowactivate --sync "$WID" 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    set -- $POINTER
    xdotool mousemove $((X + $1)) $((Y + $2))
    sleep 2
fi

sleep 35

mkdir -p "$SHOTS"
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    import -window "$WID" "$SHOTS/dd_$TAG.png" 2>/dev/null
    echo "shot: _shots/dd_$TAG.png"
else
    echo "no window (process gone?)"
fi

echo "--- driver trace -------------------------------------------------"
awk '/UIDRV|MessageBoxA:/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ------------------------------------------------------"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"
awk '/==== END ====/ {f=1} END {print (f ? "frames: STILL PUMPING" : "frames: STOPPED")}' "$ROOT/build/Orbiter.log"

pkill -x Orbiter
