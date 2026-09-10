#!/bin/bash
# Survey: does each input class actually DO something in the simulation?
# Keyboard, mouse buttons on a cockpit control, mouse wheel zoom, right-drag.
source "$(dirname "$0")/drive.sh" "Delta-glider/Virtual cockpit" /tmp/drive.txt

start_orbiter
mv 636 344
shot sv_00_vc

echo "--- keyboard: F8 cycles cockpit mode (VC -> glass -> 2D -> VC) ---"
key F8; shot sv_01_f8a
key F8; shot sv_02_f8b
key F8; shot sv_03_f8c

echo "--- keyboard: F1 toggles internal/external view ---"
key F1; shot sv_04_f1
key F1; shot sv_05_f1back

echo "--- wheel: FoV zoom in then out ---"
mv 636 344
wheel 5;  shot sv_06_wheel_in
wheel -5; shot sv_07_wheel_out

echo "--- left-click the VC HUD MODE buttons: DOCK then SRFCE ---"
mv 660 557; click l; shot sv_08_dock
mv 636 557; click l; shot sv_09_srfce

echo "--- right-drag: cockpit pan, and input still alive afterwards ---"
drag r 636 344 636 180
shot sv_10_rdrag
key F1; shot sv_11_after_drag_key   # proves the drag did not wedge the input
key F1

stop_injector
