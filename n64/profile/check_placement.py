#!/usr/bin/env python3
"""Check that every input section icache_order.ld places sits at its planned
address in the linked build.

The ordering file pins each hot function with `. = MAX(., OFFSET)`: when a
function grows past the gap before the next pinned one, ld silently moves
that one and every later one, and their tuned cache positions are lost (in
one case the two hottest interpreter functions moved and the CPU stage
lost 5 ms). This reports each placed section that is not where the file
puts it, and which section outgrew its slot, so a timing run is never made
on a shifted layout by accident.

Addresses come from the linker map, by input section name: a placed section
need not be named after the symbol in it (a renamed hot copy,
.text.unlikely.*).

  check_placement.py MAP ORDERING_LD [--text 0x80000400]

Exit status 1 when a section moved (the Makefile prints it as a warning, or
stops the build with PLACEMENT_STRICT=1).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icache_order import read_map_sections  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("ordering")
    ap.add_argument("--text", type=lambda s: int(s, 0), default=0x80000400,
                    help="start address of the .text output section (n64.ld)")
    a = ap.parse_args()

    planned = []          # (section name, planned address), in file order
    offset = None
    for line in open(a.ordering):
        m = re.match(r"\s*\. = MAX\(\., (0x[0-9a-fA-F]+)\);", line)
        if m:
            offset = int(m.group(1), 16)
            continue
        m = re.match(r"\s*\*\((\.text\.[^)*\s]+)\)", line)
        if m and offset is not None:
            planned.append((m.group(1), a.text + offset))
            offset = None

    first = {}            # section name -> (lowest address, size)
    for addr, (name, size, _) in read_map_sections(a.map).items():
        if size and (name not in first or addr < first[name][0]):
            first[name] = (addr, size)

    here = [(n, want) + first[n] for n, want in planned if n in first]
    moved = [h for h in here if h[2] != h[1]]
    # A section that runs past the next planned address is what moves the rest.
    over = [(n, size, nxt - want) for (n, want, at, size), (_, nxt, _, _)
            in zip(here, here[1:]) if at == want and want + size > nxt]
    if moved:
        n, want, at, _ = min(moved, key=lambda h: h[1])
        print("placement: %d of %d placed functions are not at their planned address; "
              "the first is %s (planned 0x%x, linked 0x%x). Timings of this build are "
              "not comparable: regenerate icache_order.ld (docs/n64/TODO.md)."
              % (len(moved), len(here), n[6:], want, at))
        for n, size, room in over[:8]:
            print("placement:   %s is %d bytes in a %d-byte slot" % (n[6:], size, room))
        return 1
    tight = min(((nxt - want - size, n) for (n, want, _, size), (_, nxt, _, _)
                 in zip(here, here[1:])), default=(0, ""))
    print("placement: all %d placed functions at their planned addresses (%d not in this "
          "build; least room to grow: %d bytes, %s)"
          % (len(here), len(planned) - len(here), tight[0], tight[1][6:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
