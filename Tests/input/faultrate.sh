#!/bin/bash
# How often does the vkQueueSubmit: -4 device fault happen at session start?
#
# Seen ONCE in twelve runs on 2026-09-08 and not reproduced, which is the worst
# state for a fault to be in -- A15's lesson. This measures the rate rather
# than trying to catch it by luck, so a later "it is fixed" claim has a
# denominator to be judged against.
#
# A run is short on purpose: the fault appeared during startup, so the cheapest
# iteration is start, let it settle, look, kill.
set -u

# DISPLAY/XAUTHORITY/WAYLAND_DISPLAY resolved from the RUNNING session.
# The cookie filename changes at every login, so a literal here survives
# exactly until the next reboot and then reports "no window".
. "$(dirname "$0")/session.sh"
N="${1:-10}"
SCN="${2:-Delta-glider/Virtual cockpit}"
BUILD=/home/racerx/orbiter-native/build
faults=0; started=0; i=0

while [ $i -lt "$N" ]; do
    i=$((i+1))
    pgrep -x Orbiter | xargs -r kill; sleep 2
    LOG=/tmp/fault_$i.txt
    cd "$BUILD" || exit 1
    setsid nohup env \
        ORBITER_VK_CHECKPOINTS=1 \
        ./Orbiter "--scenariox=$SCN" --maxframes=1000000 --keeplog \
        >"$LOG" 2>&1 </dev/null & disown
    ORBPID=$!
    # Kill THIS run on any exit, including an interrupt -- by PID, so a
    # session the user started themselves is never touched. An orphaned
    # run was once found alive SIXTEEN HOURS later, sharing the GPU with
    # the user's own Orbiter when that one took a SIGSEGV in the driver --
    # which made the crash a contaminated sample. See
    # the porting notes.
    trap '[ "${KEEP:-0}" = 1 ] || kill "$ORBPID" 2>/dev/null' EXIT INT TERM
    sleep 30

    # Did it get far enough to be a valid sample?  A run that never started a
    # session cannot vote -- the same rule kbdproof.sh had to learn.
    if awk '/Scenario not found|TERMINATING/{f=1} END{exit !f}' "$LOG"; then
        echo "run $i: INVALID (scenario not found)"; continue
    fi
    if ! pgrep -x Orbiter >/dev/null; then
        echo "run $i: INVALID (process gone before the sample)"; continue
    fi
    started=$((started+1))

    if awk '/Vulkan error in vkQueueSubmit|device fault:/{f=1} END{exit !f}' "$LOG"; then
        faults=$((faults+1)); echo "run $i: FAULT  (log kept at $LOG)"
        # The checkpoint trail is the whole reason for keeping this run: it
        # names the last marker the GPU passed before the submission died,
        # which is the difference between "the device was lost" and knowing
        # which draw lost it.
        awk '/Vulkan error in vkQueueSubmit|device fault:|addr type|GPU checkpoints at|^Orbiter:   /' "$LOG" | head -30
    else
        echo "run $i: clean"
        rm -f "$LOG"
    fi
done

pgrep -x Orbiter | xargs -r kill
echo "-----"
echo "valid runs: $started   faults: $faults"
