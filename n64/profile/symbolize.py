#!/usr/bin/env python3
"""Map "PROF <pc>" samples from the N64 sampling profiler to functions.

Usage: symbolize.py <elf> <log> [--nm mips64-elf-nm] [--top 40]
"""
import argparse, bisect, collections, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("elf"); ap.add_argument("log")
ap.add_argument("--nm", default="/opt/libdragon/bin/mips64-elf-nm")
ap.add_argument("--top", type=int, default=40)
ap.add_argument("--phase", type=int, default=None,
                help="only samples from this frame stage (0 CPU, 1 PPU, 2 present)")
a = ap.parse_args()

syms = []
for line in subprocess.run([a.nm, "-n", "--defined-only", a.elf], capture_output=True,
                           text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tTwW":
        syms.append((int(parts[0], 16) & 0xffffffff, parts[2]))
syms.sort()
addrs = [s[0] for s in syms]

# Code-cache modules ("PROFMOD <base> <bytes> <name>", n64_host.c): samples
# inside one are named "cc:<routine>".
mods = []
with open(a.log, errors="replace") as f:
    for line in f:
        if line.startswith("PROFMOD "):
            p = line.split()
            mods.append((int(p[1], 16) & 0xffffffff, int(p[2]), p[3]))
mods.sort()
mod_addrs = [m[0] for m in mods]
text_end = addrs[-1] + 0x10000 if addrs else 0

def name_of(pc):
    j = bisect.bisect_right(mod_addrs, pc) - 1
    if j >= 0 and pc < mods[j][0] + mods[j][1]:
        return "cc:" + mods[j][2]
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0 or pc > text_end:
        return "?(outside .text)"
    return syms[i][1]

counts = collections.Counter()
total = 0
with open(a.log, errors="replace") as f:
    for line in f:
        if line.startswith("PROF ") and not line.startswith("PROF_"):
            p = line.split()
            if a.phase is not None and (int(p[2]) if len(p) > 2 else 0) != a.phase:
                continue
            pc = int(p[1], 16)
            counts[name_of(pc)] += 1
            total += 1
if not total:
    sys.exit("no samples")
print(f"{total} samples")
cc = sum(n for k, n in counts.items() if k.startswith("cc:"))
print(f"compiled game code (code cache): {100.0 * cc / total:.2f}%")
for name, n in counts.most_common(a.top):
    print(f"{100.0 * n / total:6.2f}%  {n:6d}  {name}")
