#!/bin/bash
# Does a click actuate a VC control?  The DG's HUD MODE cluster is the test:
# ORBIT / SRFCE / DOCK switch the head-up display, and the HUD's mode caption
# is drawn on screen, so the effect is visible without reading any state.
source "$(dirname "$0")/drive.sh" "Delta-glider/Virtual cockpit" /tmp/click.txt

start_orbiter
mv 636 344
shot cl_00_srfce

echo "--- click ORBIT ---"
mv 612 557; click l; shot cl_01_orbit
awk '/\[mouse\]/{l=$0} END{print "   " l}' /tmp/click.txt

echo "--- click DOCK ---"
mv 660 557; click l; shot cl_02_dock
awk '/\[mouse\]/{l=$0} END{print "   " l}' /tmp/click.txt

echo "--- click SRFCE ---"
mv 636 557; click l; shot cl_03_srfce
awk '/\[mouse\]/{l=$0} END{print "   " l}' /tmp/click.txt

stop_injector
