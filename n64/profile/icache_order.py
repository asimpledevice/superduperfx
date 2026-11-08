#!/usr/bin/env python3
"""Profile-guided function placement for the VR4300 instruction cache.

The VR4300 I-cache is 16 KB, direct-mapped, with 32-byte lines: two code
lines whose addresses are equal modulo 16 KB evict each other. The hot code
of this runtime is several times larger than the cache, so where the linker
happens to put each function decides how often hot lines collide.

This tool reads a PC sample profile (n64_platform_profile_* output, "PROF
<pc>" lines) and the link map of the same build, measures the heat of every
32-byte line, and places the hot functions greedily (hottest first) at the
cache offset where their hot lines overlap the least heat already placed.
It writes an ld --section-ordering-file that puts .boot first, then each
hot function at its chosen address (gaps are left unused), then the rest.

  icache_order.py ELF MAP LOG -o order.ld [--coverage 0.995]

It also prints the modelled conflict cost (sum over cache lines of heat
products between different code lines sharing a set) for the profiled
layout and for the new one.
"""
import argparse
import re
import subprocess
from collections import defaultdict

LINE = 32
SETS = 16384 // LINE


def read_symbols(elf, nm):
    out = subprocess.run([nm, "-S", "--defined-only", elf], capture_output=True,
                         text=True, check=True).stdout
    syms = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in "tT":
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    syms.sort()
    return syms


def read_map_sections(path):
    """Input .text.* sections as {address: (name, size, object)} from a GNU
    ld map."""
    secs = {}
    pending = None
    for l in open(path, errors="replace"):
        m = re.match(r"^ (\.text\.\S+)\s*$", l)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ (\.text\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", l)
        if m:
            secs[int(m.group(2), 16)] = (m.group(1), int(m.group(3), 16), m.group(4))
            pending = None
            continue
        m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", l)
        if m and pending:
            secs[int(m.group(1), 16)] = (pending, int(m.group(2), 16), m.group(3))
        pending = None
    return secs


def conflict_cost(placed):
    """placed: list of (start address, {line offset: heat}). Sum over each
    cache set of heat products between distinct code lines in that set."""
    per_set = defaultdict(list)
    for start, lines in placed:
        for off, h in lines.items():
            per_set[((start >> 5) + off) % SETS].append(h)
    cost = 0
    for hs in per_set.values():
        total = sum(hs)
        cost += (total * total - sum(h * h for h in hs)) // 2
    return cost


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("map")
    ap.add_argument("log")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--nm", default="mips64-elf-nm")
    ap.add_argument("--coverage", type=float, default=0.995,
                    help="place the hottest functions covering this share of samples")
    ap.add_argument("--runtime-only", action="store_true",
                    help="skip functions from generated (per-game) code, build/gen/")
    ap.add_argument("--max-gap", type=int, default=2048,
                    help="largest gap (bytes) left before a function under 1%% of samples")
    ap.add_argument("--text", type=lambda s: int(s, 0), default=0x80000400,
                    help="start address of the .text output section (n64.ld)")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x80004000,
                    help="address of the first placed function (after .boot)")
    a = ap.parse_args()

    syms = read_symbols(a.elf, a.nm)
    starts = [s[0] for s in syms]
    secs = read_map_sections(a.map)

    import bisect
    heat = defaultdict(lambda: defaultdict(int))  # function index -> line -> samples
    total = 0
    for l in open(a.log, errors="replace"):
        if not l.startswith("PROF "):
            continue
        pc = int(l.split()[1], 16)
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc >= syms[i][0] + max(syms[i][1], 4):
            continue
        heat[i][(pc - syms[i][0]) >> 5] += 1
        total += 1

    order = sorted(heat, key=lambda i: -sum(heat[i].values()))
    chosen, acc = [], 0
    for i in order:
        if acc >= a.coverage * total:
            break
        addr, size, name = syms[i]
        if addr not in secs or not secs[addr][0].endswith(name):
            continue  # not in its own section (e.g. assembler code): cannot move
        if a.runtime_only and "/gen/" in secs[addr][2]:
            continue
        chosen.append(i)
        acc += sum(heat[i].values())

    before = conflict_cost([(syms[i][0], heat[i]) for i in heat])

    setheat = [0] * SETS
    cursor = a.base
    placement = []
    for i in chosen:
        addr, size, name = syms[i]
        lines = heat[i]
        best = None
        share = sum(lines.values()) / max(total, 1)
        span = SETS if share >= 0.01 else max(1, a.max_gap // LINE)
        for gap in range(span):
            start = cursor + gap * LINE
            s0 = start >> 5
            cost = sum(h * setheat[(s0 + off) % SETS] for off, h in lines.items())
            if best is None or cost < best[0]:
                best = (cost, start)
                if cost == 0:
                    break
        start = best[1]
        for off, h in lines.items():
            setheat[((start >> 5) + off) % SETS] += h
        placement.append((start, secs[addr][0], i))
        cursor = start + ((secs[addr][1] + LINE - 1) & ~(LINE - 1))

    unplaced = [i for i in heat if i not in chosen]
    after = conflict_cost([(s, heat[i]) for s, _, i in placement] +
                          [(syms[i][0], heat[i]) for i in unplaced])

    with open(a.out, "w") as f:
        f.write("/* Generated by n64/profile/icache_order.py. */\n.text : {\n")
        f.write("  *(.boot)\n  . = ALIGN(16);\n")
        for start, sec, i in placement:
            # MAX: a build whose code differs slightly from the profiled one
            # shifts later functions instead of failing the link.
            # Inside an output section `.` is relative to the section start.
            f.write("  . = MAX(., 0x%x);\n  *(%s)\n" % (start - a.text, sec))
        f.write("}\n")
    print("samples %d, placed %d functions (%.1f%% of samples), %d bytes of code + gaps"
          % (total, len(placement), 100.0 * acc / max(total, 1), cursor - a.base))
    print("modelled I-cache conflict cost: profiled layout %d, new layout %d (%.0f%%)"
          % (before, after, 100.0 * after / max(before, 1)))


if __name__ == "__main__":
    main()
