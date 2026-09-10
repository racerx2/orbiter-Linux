#!/bin/bash
# Backtrace of the most recent Orbiter core dump.
# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"
echo "=== recent dumps ==="
coredumpctl list Orbiter 2>&1 | tail -5
echo "=== backtrace ==="
coredumpctl debug Orbiter --debugger=gdb --debugger-arguments="-batch -ex 'bt 40' -ex 'info registers rip' -ex 'thread apply all bt 8'" 2>&1 | tail -80
