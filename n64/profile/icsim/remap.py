#!/usr/bin/env python3
"""remap.py OLD_ELF NEW_ELF NM > remap.txt: per function, its range in the
old build and its start in the new one (physical), for icsim."""
import sys, subprocess
def syms(e, nm):
    d = {}
    for l in subprocess.run([nm, "-S", "--defined-only", e], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in "tT": d.setdefault(p[3], (int(p[0], 16), int(p[1], 16)))
    return d
o, n = syms(sys.argv[1], sys.argv[3]), syms(sys.argv[2], sys.argv[3])
for name, (a, sz) in o.items():
    if name in n and sz and n[name][1] == sz:
        print("%x %x %x" % (a & 0x7fffff, (a + sz) & 0x7fffff, n[name][0] & 0x7fffff))
