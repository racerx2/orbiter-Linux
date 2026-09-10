#!/bin/bash
# Glass cockpit MFD buttons are big and their effect is unmistakable: MNU
# replaces the whole MFD with a mode menu. If these work and the VC's do not,
# the fault is in the VC's ray cast, not in the click path.
source "$(dirname "$0")/drive.sh" "Delta-glider/Glass cockpit" /tmp/glass.txt

start_orbiter
mv 636 344
shot gl_00_start

echo "--- click MNU under the left MFD ---"
mv 261 672; click l; shot gl_01_mnu
awk '/\[mouse\]/{l=$0} END{print "   " l}' /tmp/glass.txt

echo "--- click SEL under the left MFD ---"
mv 205 672; click l; shot gl_02_sel
awk '/\[mouse\]/{l=$0} END{print "   " l}' /tmp/glass.txt

echo "--- click the left MFD's top-left side button ---"
mv 20 381; click l; shot gl_03_side
awk '/\[mouse\]/{l=$0} END{print "   " l}' /tmp/glass.txt

stop_injector
