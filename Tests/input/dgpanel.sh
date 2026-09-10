#!/bin/bash
# ===========================================================================
# dgpanel.sh -- why is the Delta-glider's registration panel black?
#
# WHAT IT MEASURES
#
# The DG composes that panel at runtime. clbkSetClassCaps copies mesh texture
# 5 (idpanel1.dds, DXT1) into a 256x256 render target; clbkPostCreation draws
# the vessel name over it with a Sketchpad. The lettering appears and the
# background does not.
#
# Because the source is COMPRESSED, clbkScaleBlt cannot take either
# BlitTexture path -- both are gated on !bSC -- and is forced onto the
# Sketchpad fallback, the only route that RENDERS. That route ends in
# `return AutoGenMips(tgt)`, which is `return true` whatever happened, so
# nothing above it can report failure.
#
# Two log lines separate the remaining possibilities:
#
#   "oapiBlt(): first Sketchpad-fallback copy"  -- the copy was issued
#   "FLUSHTRACE target 256x256"                 -- the pad reached its draw
#
#   both      -> the draw ran and produced nothing; suspect the texture bind
#   first only-> the geometry never reached Flush
#   neither   -> the DG never asked for the copy
#
# ORBITER_VK_TRACE_FLUSH is what prints the second one, and three runs by hand
# have now been started without it -- hence this script.
# ===========================================================================
set -u
ROOT=/home/racerx/orbiter-native
SCN="${SCN:-Delta-glider/Cape Canaveral}"
LOG=/tmp/dgpanel.txt
SHOTS=$ROOT/_shots

. "$(dirname "$0")/session.sh"

pkill -x Orbiter
sleep 3
cd "$ROOT/build" || exit 1
rm -f "$ROOT/build/Orbiter.log"

setsid nohup env ORBITER_VK_TRACE_FLUSH=1 ORBITER_VK_DUMPBLT=1 \
    ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >"$LOG" 2>&1 </dev/null & disown
ORBPID=$!
trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM

echo -n "waiting for the session"
for _ in $(seq 1 120); do
    if awk '/Finished initialising panels/{f=1} END{exit !f}' \
           "$ROOT/build/Orbiter.log" 2>/dev/null; then break; fi
    kill -0 "$ORBPID" 2>/dev/null || { echo; echo "FAIL: died during load"; tail -25 "$LOG"; exit 1; }
    echo -n "."; sleep 1
done
echo
sleep 12

mkdir -p "$SHOTS"
WID=$(xdotool search --name "rbiter" | tail -1)
if [ -n "$WID" ]; then
    import -window "$WID" "$SHOTS/dgpanel.png" 2>/dev/null && echo "shot: _shots/dgpanel.png"
fi

echo "--- was the copy ISSUED? -----------------------------------------"
awk '/Sketchpad-fallback copy/ {print "  "$0} END{if(!NR) print}' "$ROOT/build/Orbiter.log" | awk 'NF'
awk '/Sketchpad-fallback copy/{n++} END{if(!n) print "  NO -- clbkScaleBlt never reached the fallback"}' "$ROOT/build/Orbiter.log"

echo "--- did the pad reach its DRAW, and for which targets? -----------"
awk '/FLUSHTRACE/ {print "  "$0}' "$ROOT/build/Orbiter.log"
awk '/FLUSHTRACE/{n++} END{if(!n) print "  (no FLUSHTRACE at all -- env not honoured?)"}' "$ROOT/build/Orbiter.log"

echo "--- did any pad batch get DROPPED? -------------------------------"
awk '/dropped a batch/ {print "  "$0}' "$ROOT/build/Orbiter.log"
awk '/dropped a batch/{n++} END{if(!n) print "  none -- every Flush opened its pass"}' "$ROOT/build/Orbiter.log"

echo "--- any blit or offscreen failure at all? ------------------------"
awk '/oapiBlt\(\) .*Failed|BltError|BeginOffscreen failed|PushRenderTarget/ {print "  "$0}' "$ROOT/build/Orbiter.log"
awk '/oapiBlt\(\) .*Failed|BltError|BeginOffscreen failed/{n++} END{if(!n) print "  none"}' "$ROOT/build/Orbiter.log"

echo "--- verdict ------------------------------------------------------"
awk '
    /Sketchpad-fallback copy/ { issued = 1 }
    /FLUSHTRACE target 256x256/ { drew = 1 }
    END {
        if (!issued) {
            print "  The copy was NEVER ISSUED. The render path is innocent;"
            print "  look at oapiGetTextureHandle(exmesh_tpl, 5)."
        } else if (!drew) {
            print "  Issued, but the pad NEVER REACHED ITS DRAW for a 256x256"
            print "  target. The geometry is being lost between StretchRect"
            print "  and Flush."
        } else {
            print "  Issued AND drawn -- a 256x256 flush happened. The draw"
            print "  produced nothing visible, so the fault is in what the pad"
            print "  SAMPLED: this is the one place a Sketchpad reads a"
            print "  BC-compressed image."
        }
    }' "$ROOT/build/Orbiter.log"

pkill -x Orbiter
