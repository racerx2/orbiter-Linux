#!/bin/bash
# Run Orbiter UNDER gdb so a freeze can be broken into.
#
# /proc/sys/kernel/yama/ptrace_scope is 1 on this desk, which means gdb cannot
# attach to an already-running process from another shell -- only a PARENT may
# trace. So gdb has to start it.
#
# gdb's stdin is a FIFO held open by a parked `sleep`, so commands can be fed
# to it from any later shell. While the inferior runs, gdb ignores stdin; a
# SIGINT to GDB (not to Orbiter) stops the inferior and gets the prompt back,
# and that is what gdbbt.sh does.
#
#   gdbrun.sh ["<rdist> <phi> <theta>"]
set -u
ROOT=/home/racerx/orbiter-native
SCN="Tests/Earth from 1.35 radii"
POS="${1:-1.35 61.5 -4.9}"
FIFO=/tmp/gdbcmd
OUT=/tmp/gdbout.txt
PIDF=/tmp/gdb.pid

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"

SCNFILE="$ROOT/build/Scenarios/$SCN.scn"
cp "$ROOT/Scenarios/$SCN.scn" "$SCNFILE"
sed -i "s/^  POS .*/  POS $POS/" "$SCNFILE"

pgrep -x Orbiter | xargs -r kill; sleep 2
pkill -f 'gdb -q ./Orbiter' 2>/dev/null; sleep 1
rm -f "$FIFO" "$OUT" "$PIDF"; mkfifo "$FIFO"

# Hold the write end open forever so later `echo > $FIFO` does not send EOF.
setsid nohup sleep infinity > "$FIFO" & disown
sleep 0.5

cd "$ROOT/build" || exit 1
rm -f Orbiter.log
setsid nohup \
    gdb -q ./Orbiter < "$FIFO" > "$OUT" 2>&1 & disown
GDBPID=$!
echo "$GDBPID" > "$PIDF"

# NO CLEANUP TRAP HERE, DELIBERATELY, and it is the one exception in this
# directory. Every other harness kills its own Orbiter on exit so an
# interrupted run cannot orphan a session on the GPU. This script is the
# opposite by design: it EXISTS to leave gdb and its inferior alive after it
# returns, so a later shell can drive them through $FIFO -- gdbbt.sh sends the
# SIGINT that gets the prompt back. An EXIT trap would kill the session the
# moment setup finished and there would be nothing to debug.
#
# The lifetime is managed instead through $PIDF; kill that pid when done.
sleep 3

{
  echo "set pagination off"
  echo "set confirm off"
  echo "handle SIGPIPE nostop noprint pass"
  # QUOTED. gdb's `run` splits its argument string on whitespace like a shell
  # would, and this scenario's name has spaces in it -- unquoted it arrives as
  # four separate argv entries and Orbiter sits at the Launchpad instead.
  echo "run --scenariox=\"$SCN\" --maxframes=1000000 --keeplog"
} > "$FIFO"

echo "gdb pid $GDBPID, inferior starting; scenario POS $POS"
echo "when it freezes, run:  Tests/input/gdbbt.sh"
