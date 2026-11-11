#!/usr/bin/env python3
"""Trace-driven function placement for the VR4300 instruction cache.

icache_order.py places functions by PC-sample heat. This tool uses an exact
record of which code ran in which order: a code-line trace from ares
(the local icprof patch, $ARES_ICTRACE), the same trace the offline
simulator (icsim) replays. From it, trg builds a temporal relationship
graph (Gloy & Smith, "Procedure placement using temporal-ordering
information", 1999) over 256-byte chunks of each function: w(x, y) counts
the uses of x with y used since x's previous use, i.e. the misses x would
take if x and y shared cache sets. Functions are then placed, most
connected first, at the cache offset where the summed weight of their
chunks' overlaps with already placed code (and with code they cannot
move, such as code-cache modules on the heap) is least.

  icache_trg.py ELF MAP TRACE -o order.ld [--also ELF MAP TRACE ...]
                [--gap-cost C] [--trg BIN] [--coverage 0.995]

ELF/MAP is the build being laid out. With --also, graphs from other builds
(other games: the runtime is shared, generated code is not) are merged by
function name, each trace weighted equally, so the layout is not tuned to
one title.

Writes the same ld ordering file as icache_order.py.
"""
import argparse
import array
import os
import subprocess
import sys
import tempfile
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icache_order import read_symbols, read_map_sections  # noqa: E402

LINE, SETS, CHUNK = 32, 512, 256
LPC = CHUNK // LINE        # lines per chunk


def text_end(elf):
    out = subprocess.run(["/opt/libdragon/bin/mips64-elf-objdump" if os.path.exists(
        "/opt/libdragon/bin/mips64-elf-objdump") else "mips64-elf-objdump", "-h", elf],
        capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 4 and p[1] == ".text":
            return int(p[3], 16) + int(p[2], 16)
    return 0x80800000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("map")
    ap.add_argument("trace")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--also", nargs=3, action="append", default=[], metavar=("ELF", "MAP", "TRACE"),
                    help="another build and trace (e.g. another game) whose graph is merged "
                         "in by function name, each trace weighted equally (repeatable)")
    ap.add_argument("--nm", default="mips64-elf-nm")
    ap.add_argument("--trg", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "icsim", "trg"),
                    help="the trg binary (cc -O2 -o icsim/trg icsim/trg.c)")
    ap.add_argument("--coverage", type=float, default=0.995,
                    help="place the most connected functions covering this share of graph weight")
    ap.add_argument("--remap", nargs=2, action="append", default=[], metavar=("ELF", "OUT"),
                    help="write the planned layout as an icsim remap for the build ELF (the "
                         "target or an --also build): placed functions by name at their planned "
                         "address, the rest packed after them in their original order (repeatable)")
    ap.add_argument("--seed", type=int, default=0,
                    help="perturb the placement order (nonzero: a randomised restart)")
    ap.add_argument("--max-gap", type=int, default=2048,
                    help="largest gap (bytes) before a function under 1%% of the weight")
    ap.add_argument("--gap-cost", type=float, default=0.0,
                    help="cost charged per byte of gap, in units of graph weight per "
                         "frame-trace (trades RAM for misses; 0: least cost at any gap)")
    ap.add_argument("--text", type=lambda s: int(s, 0), default=0x80000400)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x80004000)
    a = ap.parse_args()

    syms = read_symbols(a.elf, a.nm)
    secs = read_map_sections(a.map)

    def build_graph(elf, trace, inp):
        """Graph of one build/trace. Items: ("f", name, chunk) for code in
        functions, ("x", inp, chunk) for other code (heap: fixed). Returns
        {item: {item: weight}} normalised to a total weight of 1e9."""
        isyms = read_symbols(elf, a.nm)
        ihi = max(z[0] + z[1] for z in isyms)
        linemap = array.array("I", [0xFFFFFFFF]) * (1 << 18)
        items = []
        for (addr, size, name) in isyms:
            if size == 0 or not (0x80000000 <= addr < 0x80800000):
                continue
            first = len(items)
            nl = (size + LINE - 1) // LINE
            for j in range((nl + LPC - 1) // LPC):
                items.append(("f", name, j))
            for k in range(nl):
                linemap[((addr - 0x80000000) >> 5) + k] = first + k // LPC
        fixed_first = len(items)
        for line in range(1 << 18):
            if linemap[line] == 0xFFFFFFFF and (line << 5 | 0x80000000) >= ihi:
                linemap[line] = fixed_first + (line >> 3)
        g = defaultdict(lambda: defaultdict(float))
        with tempfile.TemporaryDirectory() as td:
            mp, out = os.path.join(td, "map.bin"), os.path.join(td, "trg.txt")
            with open(mp, "wb") as f:
                linemap.tofile(f)
            subprocess.run([a.trg, trace, mp, out], check=True)
            raw = [tuple(map(int, l.split())) for l in open(out)]
        tot = sum(w for _, _, w in raw) or 1
        def key(i):
            return items[i] if i < fixed_first else ("x", inp, i - fixed_first)
        for x, y, w in raw:
            kx, ky = key(x), key(y)
            g[kx][ky] += w * 1e9 / tot
            g[ky][kx] += w * 1e9 / tot
        return g

    adj = defaultdict(lambda: defaultdict(float))
    for inp, (elf, trace) in enumerate([(a.elf, a.trace)] + [(e, t) for e, _, t in a.also]):
        for x, nb in build_graph(elf, trace, inp).items():
            for y, w in nb.items():
                adj[x][y] += w

    # Target build: function index by name, its chunk line counts.
    by_name = {}
    for i, (addr, size, name) in enumerate(syms):
        if size and 0x80000000 <= addr < 0x80800000:
            by_name.setdefault(name, i)
    def is_fixed(it):
        return it[0] == "x"

    def fixed_set(it):  # first cache set of a fixed (absolute) chunk
        return (it[2] * LPC) % SETS

    def nlines(name, j):
        i = by_name[name]
        nl = (syms[i][1] + LINE - 1) // LINE
        return min(LPC, nl - j * LPC)

    # Movable functions: in the target build, in their own input section;
    # weight = incident graph weight.
    fweight = defaultdict(float)
    for x, nb in adj.items():
        if not is_fixed(x) and x[1] in by_name:
            fweight[by_name[x[1]]] += sum(nb.values())
    total = sum(fweight.values())
    import random
    rnd = random.Random(a.seed)
    jitter = (lambda: rnd.uniform(0.7, 1.3)) if a.seed else (lambda: 1.0)
    order = sorted(fweight, key=lambda i: -fweight[i] * jitter())
    chosen, acc = [], 0
    for i in order:
        if acc >= a.coverage * total:
            break
        addr, size, name = syms[i]
        if addr not in secs or not secs[addr][0].endswith(name):
            continue
        chosen.append(i)
        acc += fweight[i]

    placed_set = {}   # function index -> first cache set of its start
    placement, cursor = [], a.base

    for i in chosen:
        name = syms[i][2]
        nl_f = (syms[i][1] + LINE - 1) // LINE
        cost = [0.0] * SETS
        for j in range((nl_f + LPC - 1) // LPC):
            nj = min(LPC, nl_f - j * LPC)
            for y, w in adj.get(("f", name, j), {}).items():
                if is_fixed(y):
                    sy, ny = fixed_set(y), LPC
                elif y[1] in by_name and by_name[y[1]] in placed_set:
                    yi = by_name[y[1]]
                    if y[2] * LPC >= (syms[yi][1] + LINE - 1) // LINE:
                        continue
                    sy = (placed_set[yi] + y[2] * LPC) % SETS
                    ny = nlines(y[1], y[2])
                else:
                    continue
                # chunk j starts at set sy + t: overlap with y's sets [sy, sy + ny)
                for t in range(-(nj - 1), ny):
                    ov = min(ny, t + nj) - max(0, t)
                    if ov > 0:
                        cost[(sy + t - j * LPC) % SETS] += w * ov / LPC
        share = fweight[i] / max(total, 1)
        span = SETS if share >= 0.01 else max(1, a.max_gap // LINE)
        size = secs[syms[i][0]][1]
        best = None
        for gap in range(span):
            start = cursor + gap * LINE
            c = cost[(start >> 5) % SETS] + a.gap_cost * gap * LINE
            if best is None or c < best[0]:
                best = (c, start)
                if c == 0:
                    break
        start = best[1]
        placed_set[i] = (start >> 5) % SETS
        placement.append((start, secs[syms[i][0]][0], i))
        cursor = start + ((size + LINE - 1) & ~(LINE - 1))

    with open(a.out, "w") as f:
        f.write("/* Generated by n64/profile/icache_trg.py. */\n.text : {\n")
        f.write("  *(.boot)\n  . = ALIGN(16);\n")
        for start, sec, _ in placement:
            f.write("  . = MAX(., 0x%x);\n  *(%s)\n" % (start - a.text, sec))
        f.write("}\n")
    planned = {syms[i][2]: start for start, _, i in placement}
    for elf, out in a.remap:
        esyms = sorted(z for z in read_symbols(elf, a.nm) if z[1] and 0x80000000 <= z[0] < text_end(elf))
        cur = cursor
        with open(out, "w") as f:
            for addr, size, name in esyms:
                if name in planned:
                    to = planned[name]
                else:
                    to = cur
                    cur += (size + LINE - 1) & ~(LINE - 1)
                f.write("%x %x %x\n" % (addr & 0x7fffff, (addr + size) & 0x7fffff, to & 0x7fffff))
    print("placed %d functions (%.1f%% of graph weight), %d bytes of code + gaps"
          % (len(placement), 100.0 * acc / max(total, 1), cursor - a.base))


if __name__ == "__main__":
    main()
