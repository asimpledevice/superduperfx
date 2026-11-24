#!/bin/bash
# The RDP renderer's picture test: a generated PPU test ROM (ppu_test_rom.py:
# random VRAM, CGRAM and OAM, a script of register settings; no game code or
# data) runs in ares with RENDER=2, which compares every frame's RDP picture
# with the software PPU's. Prints, for each scene, the lines the RDP drew,
# the software lines with their reasons (N64PPU_WHY_*), and the frames whose
# RDP lines differed.
#
#   n64/tools/ppu_test.sh [SECONDS] [SCENES] [SEED] [EXTRA MAKE FLAGS]
#
# Needs N64_INST and the ares of n64/profile/ares_run.sh. Work files go to
# $PPUTEST_DIR (default: a new directory under ${TMPDIR:-/tmp}); the build is
# the working tree's n64/ (make clean first).
set -u
SECS=${1:-240}; SCENES=${2:-}; SEED=${3:-1}; FLAGS=${4:-}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
N64="$HERE/.."
OUT=${PPUTEST_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/ppu_test.XXXXXX")}
mkdir -p "$OUT"
python3 "$HERE/ppu_test_rom.py" "$OUT/ppu.sfc" --seed "$SEED" --scenes "$SCENES" --list > "$OUT/scenes.txt" || exit 1
( cd "$N64" && make clean >/dev/null &&
  make -j"$(nproc)" JIT816=1 SCENE=1 RENDER=2 $FLAGS "EXTRA_CFLAGS=-DN64_RDP_LOG_ALL" ROM="$OUT/ppu.sfc" > "$OUT/make.log" 2>&1 ) ||
  { echo "build failed: $OUT/make.log"; exit 1; }
cp "$N64/snesrecomp_interp.z64" "$OUT/ppu.z64"
"$N64/profile/ares_run.sh" "$OUT/ppu.z64" "$OUT/run" "$SECS" >/dev/null 2>&1
python3 - "$OUT" <<'PY'
import sys, re
out = sys.argv[1]
scenes = []
total = 0
for l in open(out + '/scenes.txt'):
    m = re.match(r'\s+frame\s+(\d+)\s+(\S+)', l)
    if m: scenes.append((int(m.group(1)), m.group(2)))
    m = re.search(r'(\d+) frames a pass', l)
    if m: total = int(m.group(1))
rows = []
for l in open(out + '/run/isviewer.log', errors='replace'):
    m = re.match(r'\[rdp\] frame (\d+): rdp (\d+) sw (\d+) black (\d+) lines, why (\w+), diff (-?\d+) px', l)
    if m: rows.append(tuple(int(x, 16) if i == 4 else int(x) for i, x in enumerate(m.groups())))
# The script's frame 0 is the first frame that is not all black.
start = next((r[0] for r in rows if r[3] < 224), None)
if start is None or not total:
    print('no picture frames in the log (%s)' % out); sys.exit(1)
per = {}
for fr, rdp, sw, black, why, diff in rows:
    if fr < start: continue
    k = (fr - start) % (total + 1)   # (the script's restart takes a frame of its own)
    if k == total: continue
    name = [n for f, n in scenes if f <= k][-1]
    p = per.setdefault(name, [0, 0, 0, 0, 0, 0])
    p[0] += 1; p[1] += rdp; p[2] += sw; p[3] |= why; p[4] += 1 if diff > 0 else 0; p[5] += max(diff, 0)
print('%-20s %6s %9s %8s %5s %8s %8s' % ('scene', 'frames', 'RDP lines', 'software', 'why', 'differ', 'pixels'))
bad = 0
for f, name in scenes:
    p = per.get(name)
    if not p: print('%-20s (not reached)' % name); continue
    print('%-20s %6d %9.0f %8.0f %5x %8d %8d' % (name, p[0], p[1] / p[0], p[2] / p[0], p[3], p[4], p[5]))
    bad += p[4]
print('frames differing: %d (%s)' % (bad, out))
PY
