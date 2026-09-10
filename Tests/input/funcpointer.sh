#!/bin/bash
# Re-run the three Function entries that looked wrong in the pointer-less
# sweep, this time with the pointer inside the render window before the
# command fires. See the POINTER note in funcdrv.sh.
#
# imgui.ini is moved aside first: ImGuiCond_FirstUseEver means a saved entry
# WINS over the position the dialog computes, so a run against an ini written
# by the pointer-less sweep would reproduce that sweep's placement no matter
# where the pointer is.
set -u
ROOT=/home/racerx/orbiter-native
OUT=/tmp/funcpointer.txt
: > "$OUT"

for i in 3 4 9; do
    mv -f "$ROOT/build/imgui.ini" "$ROOT/build/imgui.ini.bak" 2>/dev/null
    echo "=================================================================" >> "$OUT"
    echo "ENTRY $i (pointer inside)" >> "$OUT"
    LOAD_FRAMES=1200 POINTER="500 300" \
        "$ROOT/Tests/input/funcdrv.sh" "@$i" "p$i" >> "$OUT" 2>&1
    echo "--- imgui.ini after ---" >> "$OUT"
    awk '/^\[Window\]/{w=$0} /Pos=|Size=/{print w"  "$0}' "$ROOT/build/imgui.ini" >> "$OUT" 2>/dev/null
    echo >> "$OUT"
done

echo "POINTER SWEEP COMPLETE" >> "$OUT"
