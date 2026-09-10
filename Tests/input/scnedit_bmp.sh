#!/bin/bash
# ===========================================================================
# scnedit_bmp.sh -- drive ScnEditor's New Vessel tab and its preview bitmap.
#
# WHAT IS UNDER TEST.  IDC_VESSELBMP, the vessel preview panel on the New
# Vessel page.  Selecting a type in the IDC_VESSELTP tree fires TVN_SELCHANGED
# -> VesselTpChanged -> UpdateVesselBmp, which does
#
#     hVesselBmp = (HBITMAP)LoadImage (ed->InstHandle(), imagename,
#                                      IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
#
# with `imagename` the ImageBmp key from the vessel .cfg -- a FILE PATH, not a
# resource id.  The shim's LoadImageA discarded its flags and fell through to
# LoadBitmapA, which returns an EMPTY-but-non-null bitmap for a string name;
# DrawVesselBmp then divided by its zero width.  Every stock vessel ships an
# ImageBmp, so this is a SIGFPE on selecting any of them.
#
# THE VERDICT LINES ARE THE TEST.  A crash shows up as `process: DEAD` with no
# `==== END ====`, because the driver only steps while the pump runs.  A pass
# needs BOTH the END marker and a non-blank preview in the screenshot -- "did
# not crash" alone would also be true of a load that silently returned NULL.
#
#   scnedit_bmp.sh <ui-script-fragment-file> [tag]
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
FRAG="${1:?usage: scnedit_bmp.sh <fragment> [tag]}"
TAG="${2:-bmp}"
LOG=/tmp/se_$TAG.txt
UISCRIPT=/tmp/se_$TAG.ui
SHOTS=$ROOT/_shots

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"
LOAD_FRAMES="${LOAD_FRAMES:-900}"

{
    echo "wait $LOAD_FRAMES"
    echo "log ==== BEGIN ===="
    echo "customcmd scenario editor"
    echo "wait 150"
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
# Kill THIS run on any exit, including an interrupt -- by PID, so a
# session the user started themselves is never touched. An orphaned
# run was once found alive SIXTEEN HOURS later, sharing the GPU with
# the user's own Orbiter when that one took a SIGSEGV in the driver --
# which made the crash a contaminated sample. See
# the porting notes.
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

sleep 75

mkdir -p "$SHOTS"
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    import -window "$WID" "$SHOTS/se_$TAG.png" 2>/dev/null
    echo "shot: _shots/se_$TAG.png"
else
    echo "no window (process gone?)"
fi

echo "--- driver trace -------------------------------------------------"
awk '/UIDRV|UiDriver|MessageBoxA:/ {print}' "$ROOT/build/Orbiter.log"
echo "--- stderr tail --------------------------------------------------"
tail -5 "$LOG"
echo "--- verdict ------------------------------------------------------"
pgrep -x Orbiter >/dev/null && echo "process: ALIVE" || echo "process: DEAD"
awk '/==== END ====/ {f=1} END {print (f ? "frames: STILL PUMPING" : "frames: STOPPED (freeze or crash)")}' "$ROOT/build/Orbiter.log"

pkill -x Orbiter
