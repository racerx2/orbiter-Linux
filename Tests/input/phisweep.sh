#!/bin/bash
# Which way does the fixed Earth camera have to look to see the sunlit side?
# Sweeps POS phi at a fixed distance and reports how much of each frame is lit.
set -u
R="${1:-1.6}"
for phi in 0 60 120 180 240 300; do
    /home/racerx/orbiter-native/Tests/input/shadowquad.sh 2 "phi$phi" "$R $phi 0" >/dev/null 2>&1
done
cd /home/racerx/orbiter-native/_shots || exit 1
python3 - <<'PY'
from PIL import Image
import glob
print('%-18s %6s %8s' % ('frame','lit','dimblue'))
for p in sorted(glob.glob('sq_phi*.png')):
    im = Image.open(p).convert('RGB').crop((0,70,1272,688)).resize((212,103))
    px = list(im.getdata()); n = len(px)
    lit = sum(1 for r,g,b in px if (r+g+b)/3 > 60)/n
    dim = sum(1 for r,g,b in px if 10 <= (r+g+b)/3 <= 45 and b > r+6)/n
    print('%-18s %6.3f %8.3f' % (p, lit, dim))
PY
