#!/bin/bash
# What windows does Orbiter actually have, and what are they called?
# drive.sh searches for "OpenOrbiter"; this shows what is really there.
# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"
if pgrep -x Orbiter >/dev/null; then echo "Orbiter RUNNING (pid $(pgrep -x Orbiter))"; else echo "Orbiter not running"; fi
echo "--- windows matching 'rbiter' ---"
for w in $(xdotool search --name "rbiter" 2>/dev/null); do
    name=$(xdotool getwindowname "$w" 2>/dev/null)
    geom=$(xdotool getwindowgeometry --shell "$w" 2>/dev/null | tr '\n' ' ')
    echo "$w  name='$name'  $geom"
done
echo "--- all windows of the Orbiter pid ---"
for w in $(xdotool search --pid "$(pgrep -x Orbiter | head -1)" 2>/dev/null); do
    name=$(xdotool getwindowname "$w" 2>/dev/null)
    geom=$(xdotool getwindowgeometry --shell "$w" 2>/dev/null | tr '\n' ' ')
    echo "$w  name='$name'  $geom"
done
