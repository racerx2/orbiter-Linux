#!/bin/bash
# Put the desk back after a harness that used drive.sh died before its
# stop_injector ran.
#
# Two things it leaves behind and both perturb the next run: the uinject
# uinput device (whose creation warps the cursor, and whose FIFO keeps it
# alive), and the pointer sitting inside the Orbiter window, where the next
# session reads it as a click or a wheel on whatever is under it. The Options
# dialog that opened by itself in the mipfix run was exactly that.
set -u
# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

pgrep -a uinject || echo "no uinject running"
pkill -x uinject
rm -f /tmp/uinject.fifo
pgrep -x Orbiter | xargs -r kill

# Park the pointer in a corner no window reaches.
xdotool mousemove 5 5
echo -n "pointer now: "; xdotool getmouselocation
