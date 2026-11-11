#!/usr/bin/env python3
"""roles.py ELF LINES TRACE [--nm NM]: attribute I-cache misses (icsim
$ICSIM_LINES output) and executed code size to code roles, by the object
file that defines each function (n64/build). Run from n64/."""
import sys, subprocess, array, bisect, collections, glob, os, argparse
ap = argparse.ArgumentParser(); ap.add_argument("elf"); ap.add_argument("lines"); ap.add_argument("trace")
ap.add_argument("--nm", default="mips64-elf-nm"); a = ap.parse_args()
ROLE = {  # object basename -> role
 'interp816': 'kernel', 'spc': 'kernel', 'interp_bridge': 'kernel', 'apu': 'kernel', 'dsp': 'kernel',
 'ppu': 'hw', 'dma': 'hw', 'snes': 'hw', 'cpu_state': 'hw', 'common_rtl': 'hw', 'common_cpu_infra': 'hw',
 'cpu_fast_map': 'hw', 'cart': 'hw', 'guest_fiber': 'hw', 'beam_frame_driver': 'hw', 'snes_other': 'hw',
 'n64_ppu_build': 'ppu', 'n64_ppu_draw': 'present', 'n64_host': 'host', 'n64_platform': 'host',
}
def objrole(path):
    b = os.path.basename(path)[:-2]
    if '/gen/' in path or '/cc/' in path: return 'gen'
    return ROLE.get(b, 'runtime-other')
fn2role = {}
for o in glob.glob('build/**/*.o', recursive=True):
    r = objrole(o)
    for l in subprocess.run([a.nm, '--defined-only', o], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in 'tTW': fn2role.setdefault(p[2], r)
syms = []
for l in subprocess.run([a.nm, '-S', '--defined-only', a.elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'tTwW':
        s = int(p[0], 16) & 0x1fffffff; syms.append((s, s + int(p[1], 16), p[3]))
syms.sort(); los = [s[0] for s in syms]; tlo, thi = syms[0][0], max(s[1] for s in syms)
def role(addr):
    if not (tlo <= addr < thi + 64): return 'modules(heap)'
    i = bisect.bisect_right(los, addr) - 1
    if i < 0 or addr >= syms[i][1] + 32: return 'libdragon/other'
    return fn2role.get(syms[i][2], 'libdragon/other')
miss = collections.Counter()
for l in open(a.lines):
    ad, n = l.split(); miss[role(int(ad, 16) & 0x1fffffff)] += int(n)
t = array.array('I'); t.frombytes(open(a.trace, 'rb').read())
lines = {}
for w in t:
    k = w & ~31
    lines[k] = lines.get(k, 0) + 1
tot = sum(lines.values()); hot = collections.Counter(); allb = collections.Counter()
for k, n in lines.items():
    r = role(k & 0x1fffffff); allb[r] += 32
    if n * 1000 >= tot // 1000: hot[r] += 32   # >= 1e-6 of accesses... see below
tm = sum(miss.values())
print("%-16s %10s %6s %10s" % ("role", "misses", "share", "touchedKB"))
for r, m in miss.most_common():
    print("%-16s %10d %5.1f%% %10.1f" % (r, m, 100.0 * m / tm, allb[r] / 1024))
