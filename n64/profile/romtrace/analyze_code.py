"""Compiled-function working set from a romtrace trace, sized from MIPS
objects or an N64 elf; LRU code-cache loads. Usage: analyze_code.py trace
binary mips_objs_or_elf [cache KB list]"""
import sys, subprocess, collections, bisect
trace, binary, mips_elf = sys.argv[1:4]
CAPS = [int(x) for x in sys.argv[4].split(",")] if len(sys.argv) > 4 else (128, 192, 256, 384, 512, 768)
syms = []
for l in subprocess.run(["nm", "-n", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in "tTwW": syms.append((int(p[0], 16), p[2]))
anchor = [a for a, n in syms if n == "rt_anchor"][0]
byaddr = {a: n for a, n in syms}
size = {}
import glob, os
objs = sorted(glob.glob(mips_elf + "/*.o")) if os.path.isdir(mips_elf) else [mips_elf]
for l in subprocess.run(["/opt/libdragon/bin/mips64-elf-nm", "-S", "--defined-only"] + objs, capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in "tTwW": size[p[3]] = int(p[1], 16)
frames = collections.defaultdict(list); missing = set()
for l in open(trace):
    if l[0] != "C": continue
    _, f, o = l.split(); n = byaddr.get(anchor + int(o), "?")
    if n not in size: missing.add(n); continue
    frames[int(f)].append(n)
allfn = set(n for v in frames.values() for n in v)
tot = sum(size[n] for n in allfn)
gen_total = sum(v for k, v in size.items() if "_M0X" in k or "_M1X" in k)
print("compiled code in build %.2f MB; functions ever run %d = %.2f MB; unmatched names %d" % (gen_total/1e6, len(allfn), tot/1e6, len(missing)))
fs = sorted(frames)
for start in (0, 300):
    per = sorted(sum(size[n] for n in set(frames[f])) for f in fs if f >= start)
    print("frames >= %d: code run per frame median %d KB, max %d KB" % (start, per[len(per)//2] >> 10, per[-1] >> 10))
    ws = [sum(size[n] for n in set(x for f in fs[i:i+60] if f >= start for x in frames[f])) for i in range(0, len(fs) - 60, 60)]
    print("   per 60-frame window: median %d KB, max %d KB" % (sorted(ws)[len(ws)//2] >> 10, max(ws) >> 10))
print("LRU code cache by bytes (frames >= 300):")
for cap_kb in CAPS:
    cap = cap_kb << 10; lru = collections.OrderedDict(); used = 0; loads = 0; lbytes = 0; ff = collections.Counter(); fb = collections.Counter()
    for f in fs:
        for n in frames[f]:
            if n in lru: lru.move_to_end(n); continue
            if f >= 300: loads += 1; lbytes += size[n]; ff[f] += 1; fb[f] += size[n]
            lru[n] = 1; used += size[n]
            while used > cap: k, _ = lru.popitem(last=False); used -= size[k]
    print("  %4d KB: %6d loads after boot = %.2f MB, frames with loads %d, worst frame %d KB" % (cap_kb, loads, lbytes/1e6, len(ff), (max(fb.values()) if fb else 0) >> 10))
