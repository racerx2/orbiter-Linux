#!/bin/bash
# Run every entry of the Function (Custom functions) menu, one fresh session
# each, through the scripted UI driver. See funcdrv.sh for how one entry is
# driven and why a real click cannot do it.
#
# ONE SESSION PER ENTRY is the whole point: an entry that kills the process or
# hangs the pump would otherwise hide every entry after it.
set -u
ROOT=/home/racerx/orbiter-native
OUT=/tmp/funcsweep.txt
: > "$OUT"

N="${N:-13}"
FIRST="${FIRST:-0}"

for i in $(seq "$FIRST" $((N - 1))); do
    echo "=================================================================" >> "$OUT"
    echo "ENTRY $i" >> "$OUT"
    # '@', not '#': the driver's script parser strips from the first '#' as a
    # comment, so '#3' arrives as an empty argument and runs nothing.
    LOAD_FRAMES=300 "$ROOT/Tests/input/funcdrv.sh" "@$i" "e$i" >> "$OUT" 2>&1
    echo >> "$OUT"
done

echo "SWEEP COMPLETE" >> "$OUT"
