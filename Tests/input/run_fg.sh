#!/bin/bash
# Run Orbiter in the FOREGROUND and report how it ends: exit status, or the
# signal that killed it. The background harnesses hide both.
#   run_fg.sh "<scenario>" [maxsystime-seconds]
SCN="${1:-Delta-glider/DG Mk4 in orbit}"
T="${2:-20}"
cd /home/racerx/orbiter-native/build || exit 1

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

./Orbiter "--scenariox=$SCN" "--maxsystime=$T" --keeplog
rc=$?
echo "---- exit status: $rc"
if [ $rc -gt 128 ]; then
    echo "---- killed by signal $((rc - 128)) ($(kill -l $((rc - 128)) 2>/dev/null))"
fi
