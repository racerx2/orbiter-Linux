#!/bin/bash
# Reproduce the dark tile-shaped patches the user sees when zooming in and out
# on Earth.
#
# In the external tracking view PageUp is TrackCamRetreat and PageDown is
# TrackCamAdvance (Keymap.cpp: OAPI_KEY_PRIOR / OAPI_KEY_NEXT), so walking the
# camera out from low orbit crosses one surface-tile resolution level after
# another. That is the "zooming" the report is about: the artefact is expected
# to depend on which levels are in view, not on the field of view.
#
#   zoom_artefact.sh <tag> [steps-per-shot] [shots]
# writes _shots/zoom_<tag>_NN.png
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TAG="${1:-a}"
STEP="${2:-4}"
# NOT "SHOTS": drive.sh defines SHOTS as the screenshot DIRECTORY, and sourcing
# it below would overwrite the count with a path.
NSHOTS="${3:-10}"

# Optional 4th argument: force Config->TerrainShadowing for this run.
# 0=None, 1=Stencil, 2=Projected (the default, and the reference's default too
# -- OVP/D3D9Client/D3D9Config.cpp:86 sets 2 as well). This is the control for
# "is the projected terrain shadow map what draws the dark quad".
#
# The client writes Config/VulkanClient.cfg itself on a clean exit, so this is
# a file it owns; it is saved and put back regardless.
TSHADOW="${4:-}"
CLIENTCFG=/home/racerx/orbiter-native/build/Config/VulkanClient.cfg
if [ -n "$TSHADOW" ]; then
    if [ -f "$CLIENTCFG" ]; then cp "$CLIENTCFG" /tmp/VulkanClient.cfg.bak
    else                         echo "__ABSENT__" > /tmp/VulkanClient.cfg.bak; fi
    restore_cfg() {
        if [ "$(head -1 /tmp/VulkanClient.cfg.bak)" = "__ABSENT__" ]; then
            mv "$CLIENTCFG" /tmp/VulkanClient.cfg.made 2>/dev/null
            echo "client cfg moved aside to /tmp/VulkanClient.cfg.made (there was none before)"
        else
            cp /tmp/VulkanClient.cfg.bak "$CLIENTCFG"
            echo "client cfg restored"
        fi
    }
    trap restore_cfg EXIT
    mkdir -p "$(dirname "$CLIENTCFG")"
    printf 'TerrainShadowing = %s\n' "$TSHADOW" > "$CLIENTCFG"
    echo "TerrainShadowing forced to $TSHADOW"
fi

# shellcheck source=drive.sh
source "$HERE/drive.sh" "Delta-glider/DG Mk4 in orbit" /tmp/zoom_artefact.txt

start_orbiter
mv 636 344
key F1                       # external tracking view
shot "zoom_${TAG}_00"

n=0
while [ $n -lt "$NSHOTS" ]; do
    n=$((n+1))
    i=0
    while [ $i -lt "$STEP" ]; do
        i=$((i+1))
        inj "key PRIOR 120"   # retreat one step
        sleep 0.35
    done
    sleep 1.0
    printf -v idx "%02d" "$n"
    shot "zoom_${TAG}_$idx"
done

stop_injector
pgrep -x Orbiter | xargs -r kill
