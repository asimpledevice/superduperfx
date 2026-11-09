#!/usr/bin/env python3
"""Check that every function icache_order.ld places sits at its planned
address in the linked ELF.

The ordering file pins each hot function with `. = MAX(., OFFSET)`: when a
function grows past the gap before the next pinned one, ld silently moves
that one and every later one, and their tuned cache positions are lost (in
one case the two hottest interpreter functions moved and the CPU stage
lost 5 ms). This reports each placed function that is not where the file
puts it, so a timing run is never made on a shifted layout by accident.

  check_placement.py ELF ORDERING_LD [--text 0x80000400] [--nm mips64-elf-nm]

Exit status 1 when a function moved (the Makefile prints it as a warning).
"""
import argparse
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("ordering")
    ap.add_argument("--text", type=lambda s: int(s, 0), default=0x80000400,
                    help="start address of the .text output section (n64.ld)")
    ap.add_argument("--nm", default="mips64-elf-nm")
    a = ap.parse_args()

    planned = []
    offset = None
    for line in open(a.ordering):
        m = re.match(r"\s*\. = MAX\(\., (0x[0-9a-fA-F]+)\);", line)
        if m:
            offset = int(m.group(1), 16)
            continue
        m = re.match(r"\s*\*\(\.text\.(\S+)\)", line)
        if m and offset is not None:
            planned.append((m.group(1), a.text + offset))
            offset = None

    out = subprocess.run([a.nm, "--defined-only", a.elf], capture_output=True,
                         text=True, check=True).stdout
    addr = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tT":
            addr.setdefault(p[2], int(p[0], 16))

    moved = [(name, want, addr[name]) for name, want in planned
             if name in addr and addr[name] != want]
    missing = [name for name, _ in planned if name not in addr]
    if moved:
        first = min(moved, key=lambda m: m[1])
        print("placement: %d of %d placed functions are not at their planned address; "
              "the first is %s (planned 0x%x, linked 0x%x): the function placed before it "
              "outgrew its gap. Timings of this build are not comparable; shrink or move "
              "that function out of line, or regenerate icache_order.ld."
              % (len(moved), len(planned), first[0], first[1], first[2]))
        return 1
    print("placement: all %d placed functions at their planned addresses (%d not in this build)"
          % (len(planned) - len(missing), len(missing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
