#!/bin/bash
# Break into the gdb started by gdbrun.sh and dump every thread's stack.
#
# The SIGINT goes to GDB, which stops the inferior and returns to its prompt.
# Signalling Orbiter itself would just kill it.
set -u
FIFO=/tmp/gdbcmd
OUT=/tmp/gdbout.txt
PIDF=/tmp/gdb.pid

[ -f "$PIDF" ] || { echo "no $PIDF -- run gdbrun.sh first"; exit 1; }
GDBPID=$(cat "$PIDF")

kill -INT "$GDBPID" 2>/dev/null
sleep 2
{
  echo "thread apply all bt 30"
  echo "info threads"
} > "$FIFO"
sleep 4
echo "===================== gdb output ====================="
cat "$OUT"
