#!/usr/bin/env python3
"""Attribute VR4300 data-cache misses to data objects.

Input: a dcprof.txt written by ares's local data-cache profiler (MISS / PAIR /
PC lines; see the build notes), the ELF of the profiled build (static data
symbols, function symbols) and the IS-Viewer log of the run ("[heap] name addr
size" lines printed by an N64_DCPROF build).

  dcache_report.py ELF LOG DCPROF [--top 25] [--nm mips64-elf-nm]

Prints misses per data object (heap objects split into 1 KB regions when
large), the object pairs that evict each other most (at 256-byte granularity)
and the functions whose loads and stores miss most.
"""
import argparse
import bisect
import re
import subprocess
from collections import Counter


def load_objects(elf, nm, log):
    objs = []
    out = subprocess.run([nm, "-S", "--defined-only", elf], capture_output=True,
                         text=True, check=True).stdout
    funcs = []
    for l in out.splitlines():
        p = l.split()
        if len(p) != 4:
            continue
        addr, size, kind, name = int(p[0], 16), int(p[1], 16), p[2], p[3]
        if kind in "tT":
            funcs.append((addr, size, name))
        elif kind in "bBdDrRsSgG" and size:
            objs.append((addr, size, name, False))
    for l in open(log, errors="replace"):
        m = re.match(r"\[heap\] (\S+) ([0-9a-f]+) (\d+)", l)
        if m:
            objs.append((int(m.group(2), 16), int(m.group(3)), m.group(1), True))
    objs.sort()
    funcs.sort()
    return objs, funcs


def finder(items):
    starts = [o[0] for o in items]

    def find(addr):
        i = bisect.bisect_right(starts, addr) - 1
        while i >= 0:
            a, size = items[i][0], items[i][1]
            if a <= addr < a + max(size, 1):
                return items[i]
            if addr - a > (1 << 24):
                break
            i -= 1
        return None
    return find


def label(find, vaddr, split_heap=True):
    o = find(vaddr)
    if not o:
        return "?%05x" % ((vaddr >> 12) & 0xfffff)
    name = o[2]
    if split_heap and o[1] > 16384:
        name += "+%dK" % ((vaddr - o[0]) >> 10)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("log")
    ap.add_argument("dcprof")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--nm", default="mips64-elf-nm")
    a = ap.parse_args()

    objs, funcs = load_objects(a.elf, a.nm, a.log)
    find_obj, find_fn = finder(objs), finder(funcs)

    miss_obj, miss_whole, pairs, pcs = Counter(), Counter(), Counter(), Counter()
    total = 0
    for l in open(a.dcprof):
        p = l.split()
        if p[0] == "MISS":
            v = int(p[1], 16) | 0x80000000
            n = int(p[2])
            total += n
            miss_obj[label(find_obj, v)] += n
            miss_whole[label(find_obj, v, False)] += n
        elif p[0] == "PAIR":
            old = (int(p[1], 16) << 8) | 0x80000000
            new = (int(p[2], 16) << 8) | 0x80000000
            pairs[(label(find_obj, old), label(find_obj, new))] += int(p[3])
        elif p[0] == "PC":
            f = find_fn(int(p[1], 16))
            pcs[f[2] if f else p[1]] += int(p[2])

    print("data-cache misses: %d" % total)
    print("\nby object:")
    for k, v in miss_whole.most_common(a.top):
        print("  %6.2f%% %9d  %s" % (100.0 * v / total, v, k))
    print("\nby region (large objects in 1 KB steps):")
    for k, v in miss_obj.most_common(a.top):
        print("  %6.2f%% %9d  %s" % (100.0 * v / total, v, k))
    print("\nevictions, evicted <- by (256-byte granularity):")
    for (o, n), v in pairs.most_common(a.top):
        print("  %6.2f%% %9d  %s <- %s" % (100.0 * v / total, v, o, n))
    print("\nby function (missing loads/stores):")
    for k, v in pcs.most_common(a.top):
        print("  %6.2f%% %9d  %s" % (100.0 * v / total, v, k))


if __name__ == "__main__":
    main()
