#!/bin/bash
# ===========================================================================
# funcdrv.sh -- run ONE entry of the Function (Custom functions) menu through
# the scripted UI driver, in a fresh session.
#
# WHY THIS REPLACES funcmenu.sh.  funcmenu.sh aims a real pointer at a pixel
# and clicks.  Measured on this desk, that does not work: an injected press
# reaches ImGui as HOVER but never as a PRESS -- neither uinject's held button
# nor `xdotool click` produces a `btn=` in the input trace -- so the entry was
# never actually run and only the user's own hand could test these.
#
# The driver runs the command through DlgFunction's own call, from inside
# orbiter_PumpFrame, which is exactly where a real click runs it: same thread,
# same point in the frame, same re-entrancy.  See the note above
# DlgFunction::CmdRun.
#
#   funcdrv.sh <label-substring | @index | -list> [tag]
#
# THE FREEZE TEST IS THE 'SWEEP END' MARKER.  The driver only steps when the
# pump runs, so if the command deadlocks the frame pump -- which is what
# TerrainToolKit did -- no later step is ever reached and END never appears.
# A run that prints END kept drawing frames after the command returned.
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
WANT="${1:--list}"
TAG="${2:-$(echo "$WANT" | tr -c 'A-Za-z0-9' '_')}"
LOG=/tmp/fd_$TAG.txt
UISCRIPT=/tmp/fd_$TAG.ui
SHOTS=$ROOT/_shots

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

# LOAD_FRAMES: pump frames that elapse before the session is up and the
# modules have registered their commands. Measured with `-list`, which dumps
# the table four times across the run so the first non-empty dump shows when
# registration happened.
LOAD_FRAMES="${LOAD_FRAMES:-900}"

if [ "$WANT" = "-list" ]; then
    cat > "$UISCRIPT" <<EOF
wait 100
log ==== DUMP A ====
customdump
wait 400
log ==== DUMP B ====
customdump
wait 400
log ==== DUMP C ====
customdump
wait 400
log ==== DUMP D ====
customdump
log ==== SWEEP END ====
EOF
else
    cat > "$UISCRIPT" <<EOF
wait $LOAD_FRAMES
log ==== SWEEP BEGIN ====
customdump
wait 60
customcmd $WANT
wait 300
dump after-cmd
log ==== SWEEP END ====
EOF
fi

pgrep -x Orbiter | xargs -r kill
sleep 3
cd "$ROOT/build" || exit 1

# --keeplog APPENDS. Without this the run reads the PREVIOUS run's trace.
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

# PUT THE POINTER IN THE WINDOW BEFORE THE COMMAND RUNS, when asked.
#
# Not cosmetic. ImGuiNote::Display and DlgExtMFD::Display -- both pristine
# upstream -- open with
#
#     ImVec2 pos = ImGui::GetMousePos() + ImVec2(-10, 10);
#     ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
#
# ("position the window just below the cursor"). With the pointer outside the
# window ImGui's mouse position is invalid, the window is placed far off to
# the left and clamped back to a ~19px sliver at x=0 -- which looks exactly
# like a broken window and is not one. A real click always has the pointer
# inside. POINTER="cx cy" reproduces that; xdotool POSITION is the half of
# synthetic input that works on this desk (see drive.sh).
#
# LOAD_FRAMES must be large enough that the command fires AFTER this move.
sleep 40
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ] && [ -n "${POINTER:-}" ]; then
    xdotool windowactivate --sync "$WID" 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    set -- $POINTER
    xdotool mousemove $((X + $1)) $((Y + $2))
    echo "pointer moved to window+($1,$2)"
    sleep 2
fi

sleep 35

mkdir -p "$SHOTS"
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    import -window "$WID" "$SHOTS/fd_$TAG.png" 2>/dev/null
    echo "shot: _shots/fd_$TAG.png"
else
    echo "no window (process gone?)"
fi

echo "--- driver trace -------------------------------------------------"
awk '/UIDRV|UiDriver|MessageBoxA:/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ------------------------------------------------------"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"
awk '/SWEEP END|DUMP D/ {found=1} END {print (found ? "frames: STILL PUMPING (no freeze)" : "frames: STOPPED (freeze or crash)")}' "$ROOT/build/Orbiter.log"

pgrep -x Orbiter | xargs -r kill
