#!/bin/bash
# Is the dark quad over Earth the PROJECTED TERRAIN SHADOW MAP?
#
# Two runs of one fixed-camera scenario -- no key input, no drift, so the two
# frames are directly comparable -- with Config->TerrainShadowing forced to
# 2 (Projected, the default here and in the D3D9 reference) and to 0 (None).
#
#   shadowquad.sh <mode 0|1|2> <tag>
# writes _shots/sq_<tag>.png
set -u
MODE="${1:-2}"
TAG="${2:-t$MODE}"
RDIST="${3:-}"          # optional: camera distance in Earth radii
CLDSHD="${4:-}"         # optional: force Orbiter.cfg EnableCloudShadows TRUE/FALSE
SCN="Tests/Earth from 1.35 radii"
ROOT=/home/racerx/orbiter-native
CLIENTCFG=$ROOT/build/Config/VulkanClient.cfg
SCNFILE="$ROOT/build/Scenarios/$SCN.scn"

if [ -n "$RDIST" ]; then
    cp "$ROOT/Scenarios/$SCN.scn" "$SCNFILE"
    # RDIST is the whole POS triple: "<radii> <phi-deg> <theta-deg>". phi/theta
    # matter as much as the distance -- at phi=0 this camera looks at the NIGHT
    # side, and the near-straight vertical edge there is the terminator, not
    # the artefact. Learned the hard way.
    sed -i "s/^  POS .*/  POS $RDIST/" "$SCNFILE"
    echo "camera POS $RDIST"
fi

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

# The client writes this file itself on a clean exit, so it is a file it owns.
# Saved and put back regardless.
if [ -f "$CLIENTCFG" ]; then cp "$CLIENTCFG" /tmp/vkclient.cfg.bak; HAD=1
else                         HAD=0; fi
ORBCFG=$ROOT/build/Orbiter.cfg
cp "$ORBCFG" /tmp/orbiter.cfg.sqbak
restore() {
    if [ "$HAD" = 1 ]; then cp /tmp/vkclient.cfg.bak "$CLIENTCFG"
    else                    mv "$CLIENTCFG" /tmp/vkclient.cfg.made 2>/dev/null; fi
    cp /tmp/orbiter.cfg.sqbak "$ORBCFG"
    echo "client cfg and Orbiter.cfg put back"
}
trap restore EXIT

if [ -n "$CLDSHD" ]; then
    # EnableCloudShadows: the SHIPPED DEFAULT is false (Config.cpp:91) and this
    # desk has it TRUE. Cloud shadows are the other thing that darkens a patch
    # of terrain, so this is the second control.
    sed -i "s/^EnableCloudShadows *=.*/EnableCloudShadows = $CLDSHD/" "$ORBCFG"
    awk '/EnableCloudShadows/ {print "  " $0}' "$ORBCFG"
fi
SMM="${5:-}"            # optional: force Config->ShadowMapMode (0 = no shadow maps at all)
mkdir -p "$(dirname "$CLIENTCFG")"
printf 'TerrainShadowing = %s\n' "$MODE" > "$CLIENTCFG"
echo "TerrainShadowing = $MODE"
if [ -n "$SMM" ]; then
    printf 'ShadowMapMode = %s\n' "$SMM" >> "$CLIENTCFG"
    echo "ShadowMapMode = $SMM"
fi
EXTRA="${6:-}"          # optional: extra VulkanClient.cfg lines, ';'-separated
if [ -n "$EXTRA" ]; then
    echo "$EXTRA" | tr ';' '\n' >> "$CLIENTCFG"
    echo "extra: $EXTRA"
fi

pgrep -x Orbiter | xargs -r kill; sleep 2
cd "$ROOT/build" || exit 1
setsid nohup ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
    >/tmp/sq_$TAG.txt 2>&1 </dev/null &
disown
sleep 40

if ! pgrep -x Orbiter >/dev/null; then echo "NOT RUNNING (see /tmp/sq_$TAG.txt)"; exit 1; fi
WID=$(xdotool search --name "rbiter" | tail -1)
mkdir -p "$ROOT/_shots"
import -window "$WID" "$ROOT/_shots/sq_$TAG.png" && echo "shot: _shots/sq_$TAG.png"
pgrep -x Orbiter | xargs -r kill
