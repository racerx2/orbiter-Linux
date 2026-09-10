#!/bin/bash
# A publicity shot: the Delta-glider in orbit, Earth's limb, and the stars.
#
# THREE OVERLAYS HAVE TO COME OFF, and all three are named in the source:
#
#   * Planetarium mode -- constellation lines, the celestial grid and its
#     labels. Keymap.cpp:212 binds F9 to "TogglePlanetarium". Orbiter.cfg
#     carries `Planetarium = 451`, whose bit 0 is the enable (Config.h:145).
#
#   * The body force vectors, the G/L/D readouts across the hull. There is NO
#     key for these -- the complete logical-key table in Keymap.cpp offers only
#     Ctrl+F9 "DlgVisualHelpers" and F6 "DlgOptions". They are a persisted
#     setting: Config.cpp:719 reads `Bodyforces = <flags> <scale> <opacity>`,
#     and graphicsapi.h:314 gives bit 0 as BFV_ENABLE. This desk's config says
#     63, i.e. enable|logscale|weight|thrust|lift|drag. Clearing bit 0 gives 62.
#
#     Note the DEFAULT (Config.cpp:136) is BFV_WEIGHT|BFV_THRUST|BFV_LIFT|
#     BFV_DRAG with no BFV_ENABLE -- so the vectors are off in a fresh install
#     and someone turned them on here.
#
# The config edit is made on a COPY-AND-RESTORE basis: the original file is put
# back at the end whatever happens, so the user's settings are as they were.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CFG=/home/racerx/orbiter-native/build/Orbiter.cfg
BAK=/tmp/Orbiter.cfg.shotbak

cp "$CFG" "$BAK" || exit 1
restore() { cp "$BAK" "$CFG"; echo "Orbiter.cfg restored"; }
trap restore EXIT

python3 - "$CFG" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p).read()
def clear_bit0(m):
    n = int(m.group(2))
    return m.group(1) + str(n & ~1) + m.group(3)
s2 = re.sub(r'(Bodyforces\s*=\s*)(\d+)(.*)', clear_bit0, s)
s2 = re.sub(r'(Planetarium\s*=\s*)(\d+)()',  clear_bit0, s2)
open(p, 'w').write(s2)
for line in s2.splitlines():
    if line.startswith(('Bodyforces', 'Planetarium')):
        print('   ', line)
PY

# shellcheck source=drive.sh
source "$HERE/drive.sh" "Delta-glider/DG Mk4 in orbit" /tmp/orbit_shot.txt

start_orbiter
mv 636 344
key F1                 # external tracking view
shot orb_b00

for i in 01 02 03 04 05 06 07 08 09 10 11 12; do
    drag r 636 400 636 365
    shot "orb_b$i"
done

stop_injector
pgrep -x Orbiter | xargs -r kill
