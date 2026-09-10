#!/bin/bash
# ===========================================================================
# iconsweep.sh -- open every icon-captioned dialog in turn and photograph each.
#
# WHY IT IS TIMED RATHER THAN BATCHED. The core dialogs are ImGui windows and
# every one of them opens at the same default position, so opening several in
# one run stacks them exactly on top of each other and a single screenshot
# shows only the last. There is also no driver op that closes an ImGuiDialog
# (`cancel` posts IDCANCEL to a shim dialog; these are not shim dialogs).
#
# So: the UI script opens one dialog every WAIT frames, and the shell grabs a
# frame on the same cadence. Each grab therefore catches a different dialog on
# top of the pile, and the title bar -- which is what carries the FontAwesome
# glyph under test -- is always visible.
#
# What is under test: every caption is built as ICON_FA_<x> " Orbiter: ...",
# and a codepoint missing from the font actually in use draws as ImGui's
# fallback glyph '?'. See the porting notes.
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
TAG="${1:-icons}"
LOG=/tmp/is_$TAG.txt
UISCRIPT=/tmp/is_$TAG.ui
SHOTS=$ROOT/_shots

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

# One dialog per WAIT frames. The grab loop does NOT sleep a matching number
# of seconds -- that was the first attempt and it failed: the driver steps once
# per FRAME, this desk runs the session at ~165 fps, so 480 frames is under
# three seconds and all seven dialogs had opened before the first grab. Every
# screenshot then showed the same top-of-pile window.
#
# The loop below waits for each dialog's own "-- returned" line to appear in
# Orbiter.log instead, so it is tied to the driver's actual progress and not to
# a guess about frame rate.
WAIT=3000

# menucmd label, then the tag for its screenshot.
ITEMS=(
    "Ship:focus"
    "Camera:camera"
    "Speed:tacc"
    "Map:map"
    "Record:recorder"
    "Options:options"
    "Function:function"
)

{
    echo "wait 900"
    echo "log ==== BEGIN ===="
    for it in "${ITEMS[@]}"; do
        echo "menucmd ${it%%:*}"
        echo "wait $WAIT"
    done
    echo "log ==== END ===="
} > "$UISCRIPT"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log" imgui.ini

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

# Wait out the load, then put the pointer in the window: several of these
# position themselves at ImGui::GetMousePos() on first use.
sleep 32
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    xdotool windowactivate --sync "$WID" 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$WID")"
    xdotool mousemove $((X + 600)) $((Y + 300))
fi
sleep 4

# GRAB ON THE COUNT OF COMMANDS RUN, not on a per-label search and not on a
# clock. Searching for a label only asks "has it EVER run", which is true for
# every earlier dialog the moment the loop starts late -- the second attempt
# failed exactly that way, racing through the first five grabs and catching
# whatever happened to be on top.
#
# The Nth grab therefore waits for the Nth "-- returned" line. Re-issuing a
# menu command is safe and is what makes this work: EnsureEntry calls
# Activate() on a dialog that already exists, so the named one comes to the
# front even though all of them stay open.
mkdir -p "$SHOTS"
n=0
for it in "${ITEMS[@]}"; do
    label="${it%%:*}"
    tag="${it##*:}"
    n=$((n + 1))

    ok=0
    for _ in $(seq 1 120); do
        c=$(awk 'index($0,"menucmd") && index($0,"-- returned"){n++} END{print n+0}' \
            "$ROOT/build/Orbiter.log" 2>/dev/null)
        [ "${c:-0}" -ge "$n" ] && { ok=1; break; }
        sleep 1
    done
    [ "$ok" = 0 ] && { echo "TIMEOUT waiting for #$n ($label)"; continue; }

    sleep 2   # let the activated dialog reach the front and draw
    WID=$(xdotool search --name "rbiter" | tail -1)
    [ -z "$WID" ] && { echo "window gone at $tag"; break; }
    import -window "$WID" "$SHOTS/ic_$tag.png" 2>/dev/null
    echo "shot: _shots/ic_$tag.png  ($label, cmd #$n)"
done

echo "--- driver trace ---"
awk '/UIDRV menucmd/ {print}' "$ROOT/build/Orbiter.log"
echo "--- verdict ---"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"
pkill -x Orbiter
