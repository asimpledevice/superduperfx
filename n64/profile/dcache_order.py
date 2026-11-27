#!/usr/bin/env python3
"""Pin the hottest static variables for the VR4300 data cache.

The data cache is 8 KB, direct-mapped, 16-byte lines. Static variables are
laid out in object-file order, so adding one variable anywhere moves every
later one to a different cache position, and two hot variables (or a hot
variable and the hot part of the stack) that end up 8 KB apart evict each
other. In practice that swung frame times by 25% from unrelated changes.

This tool takes an access profile (ACC lines from ares's local data-cache
profiler, see ares-dcache-profiler.patch), the ELF and the link map, picks
the hottest small static variables and writes ld --section-ordering-file
statements that put them in a compact block at an 8 KB-aligned start of
their output section (.sdata, .sbss, .data, .bss, .rodata: constant
tables collide like variables). The blocks take
consecutive cache positions, so pinned variables never collide with each
other, and their positions no longer depend on the rest of the program.

  dcache_order.py ELF MAP DCPROF -o data_order.ld [--max-size 2048]

Append the output to the instruction ordering file (one ordering file per
link): icache_order.ld holds both.
"""
import argparse
import bisect
import re
import subprocess

SECTIONS = (".sdata", ".sbss", ".data", ".bss", ".rodata")


def profile(elf, mp, dcprof, nm):
    """One build's profile: the input section and size of each variable and
    its share of the profiled accesses, by variable name."""
    # Input data sections from the map: address -> (section, size).
    insec = {}
    pending = None
    for l in open(mp, errors="replace"):
        m = re.match(r"^ ((?:\.sdata|\.sbss|\.data|\.bss|\.rodata)\.\S+)\s*$", l)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ ((?:\.sdata|\.sbss|\.data|\.bss|\.rodata)\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s", l)
        if m:
            insec[int(m.group(2), 16)] = (m.group(1), int(m.group(3), 16))
            pending = None
            continue
        m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S", l)
        if m and pending:
            insec[int(m.group(1), 16)] = (pending, int(m.group(2), 16))
        pending = None

    out = subprocess.run([nm, "-S", "--defined-only", elf], capture_output=True,
                         text=True, check=True).stdout
    objs = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in "bBdDsSgGrR":
            objs.append((int(p[0], 16), int(p[1], 16), p[3]))
    objs.sort()
    starts = [o[0] for o in objs]

    acc = {}
    total = 0
    for l in open(dcprof):
        p = l.split()
        if p[0] != "ACC":
            continue
        v = int(p[1], 16) | 0x80000000
        c = int(p[2])
        total += c
        # A profiled address is a 16-byte line: credit every object in it
        # (a small table can share its line with a cold string).
        i = bisect.bisect_right(starts, v + 15) - 1
        while i >= 0 and objs[i][0] + max(objs[i][1], 1) > v:
            acc[i] = acc.get(i, 0) + c
            i -= 1

    share = {}
    where = {}
    for i, c in acc.items():
        addr, size, name = objs[i]
        if addr in insec:
            share[name] = share.get(name, 0.0) + c / max(total, 1)
            where[name] = (insec[addr][0], insec[addr][1], size)
    return share, where


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("map")
    ap.add_argument("dcprof")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--nm", default="mips64-elf-nm")
    ap.add_argument("--max-size", type=int, default=2048,
                    help="largest variable to pin (bytes)")
    ap.add_argument("--min-share", type=float, default=0.0005,
                    help="smallest share of static accesses to pin")
    ap.add_argument("--also", nargs=3, action="append", default=[], metavar=("ELF", "MAP", "DCPROF"),
                    help="another build and its profile (another game): accesses are merged by "
                         "variable name, each profile weighted equally (repeatable)")
    ap.add_argument("--keep", help="an ordering file whose .rodata block (placed by hand) is "
                                   "kept in place of the generated one")
    ap.add_argument("--start", type=lambda v: int(v, 0), default=0,
                    help="cache position (0..8191) of the first pinned variable: after what "
                         "is placed by hand (--keep) and the hot stack frames")
    ap.add_argument("--budget", type=int, default=6144,
                    help="bytes of cache positions the pinned blocks may use")
    a = ap.parse_args()

    share, where = profile(a.elf, a.map, a.dcprof, a.nm)
    for elf, mp, prof in a.also:
        sh, _ = profile(elf, mp, prof, a.nm)
        for name, v in sh.items():
            if name in where:      # (only what the build being laid out has)
                share[name] = share.get(name, 0.0) + v
    nprof = 1 + len(a.also)

    picks = {s: [] for s in SECTIONS}
    used = 0
    for name in sorted(share, key=lambda n: -share[n] / max(where[n][2], 1)):
        sec, size = where[name][:2], where[name][2]
        if size > a.max_size or share[name] < a.min_share * nprof:
            continue
        out_sec = sec[0][:sec[0].index(".", 1)]
        if out_sec not in picks or used + sec[1] > a.budget:
            continue
        picks[out_sec].append((sec[0], sec[1], name, share[name]))
        used += (sec[1] + 7) & ~7

    kept = ""
    if a.keep:
        text = open(a.keep).read()
        kept = text[text.index(".rodata : {"):] if ".rodata : {" in text else ""
    with open(a.out, "w") as f:
        f.write("/* Generated by n64/profile/dcache_order.py: hot static data, pinned. */\n")
        colour = a.start
        for s in SECTIONS:
            if not picks[s] or (kept and s == ".rodata"):
                continue
            f.write("%s : {\n  . = ALIGN(8192);\n" % s)
            if colour:
                f.write("  . += 0x%x;\n" % colour)
            for sec, size, name, _ in picks[s]:
                f.write("  *(%s)\n" % sec)
                colour += (size + 7) & ~7
            f.write("}\n")
            colour = (colour + 15) & ~15
        f.write(kept)
    n = sum(len(v) for v in picks.values())
    got = sum(x[3] for v in picks.values() for x in v)
    print("pinned %d variables, %d bytes of cache positions, %.1f%% of the profiled accesses"
          % (n, colour - a.start, 100.0 * got / nprof))


if __name__ == "__main__":
    main()
