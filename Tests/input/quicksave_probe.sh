#!/bin/bash
# Does the menu bar's Save button actually write a quicksave?
#
# Orbiter::Quicksave builds "Quicksave\<scenario leaf> <n>" and hands it to
# SaveScenario, which opens an ofstream on ScnPath(that). The leaf is taken by
# a basename scan that tested only for a backslash, so on Linux -- where a
# scenario name is composed with '/' -- nothing was stripped and the target
# was Scenarios/Quicksave/<folder>/<name> NNNN.scn, a directory that does not
# exist. The save then failed apart from an on-screen notification.
#
# Clicks the Save button in the menu bar and reports what appeared under
# Scenarios/Quicksave.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=drive.sh
source "$HERE/drive.sh" "Delta-glider/Cape Canaveral" /tmp/quicksave_probe.txt

QSDIR=/home/racerx/orbiter-native/build/Scenarios/Quicksave

echo "=== before ==="
ls -1 "$QSDIR" 2>/dev/null || echo "(no Quicksave directory)"

start_orbiter

# The Save button in the menu bar, in client coordinates read off a screenshot
# of this window size. The menu bar is an auto-hiding strip: it is drawn only
# while the pointer is near the top edge, so the pointer is walked up to it
# and left there for a moment before the click rather than jumped straight on
# to the button.
mv 724 200
mv 724 90
mv 724 30
sleep 1
echo "X pointer now: $(xdotool getmouselocation)"
echo "Orbiter saw:   $(awk '/\[mouse\]/{l=$0} END{print l}' /tmp/quicksave_probe.txt)"
shot quicksave_hover
click l
sleep 1
shot quicksave_click

stop_injector
pgrep -x Orbiter | xargs -r kill

echo "=== after ==="
ls -1 "$QSDIR" 2>/dev/null || echo "(no Quicksave directory)"
