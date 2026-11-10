"""ROM page working set from a romtrace trace: static code share (from the
recompiler manifest, LoROM), who reads each page, per-frame pages, LRU cache
faults. Usage: analyze_rom.py unused trace binary program_manifest.json"""
import sys, json, subprocess, collections, bisect
S = sys.argv[1]; trace = sys.argv[2]; binary = sys.argv[3]; manifest = sys.argv[4]
PG = 4096
syms = []
for l in subprocess.run(["nm", "-n", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in "tTwW": syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [a for a, _ in syms]
anchor = [a for a, n in syms if n == "rt_anchor"][0]
def sym(off):
    i = bisect.bisect_right(addrs, anchor + off) - 1
    return syms[i][1] if i >= 0 else "?"
def kind(name):
    if name.startswith("dma_"): return "dma"
    if "bridge_bus_read" in name or name.startswith("interp"): return "interp"
    if name[-5:-3] == "_M" or "_M0X" in name or "_M1X" in name: return "native"
    return "rt:" + name
rom = 0; frames = []; cur = None; touch = collections.defaultdict(set); first_frame = {}
calls = []
for l in open(trace):
    p = l.split()
    if p[0] == "ROM": rom = int(p[1])
    elif p[0] == "F": cur = (int(p[1]), []); frames.append(cur)
    elif p[0] == "R":
        f, pg, rip = int(p[1]), int(p[2]), int(p[3])
        cur[1].append(pg); touch[pg].add(kind(sym(rip))); first_frame.setdefault(pg, f)
npages = rom // PG
# static code: union of manifest node ranges (LoROM)
m = json.load(open(manifest))
code = bytearray(rom)
def off(pc24):
    b, a = pc24 >> 16, pc24 & 0xffff
    return ((b & 0x7f) << 15) | (a & 0x7fff) if a >= 0x8000 else None
for n in m["nodes"].values():
    lo, hi = off(n["min_pc24"]), off(n["max_pc24"])
    if lo is None or hi is None or hi < lo: continue
    for x in range(lo, min(hi + 4, rom)): code[x] = 1
codepg = [sum(code[i*PG:(i+1)*PG]) / PG for i in range(npages)]
print("ROM %d KB, %d pages of 4 KB; static code bytes %d KB (%.0f%%)" % (rom >> 10, npages, sum(code) >> 10, 100 * sum(code) / rom))
never = [p for p in range(npages) if p not in touch]
print("pages never read in %d frames: %d (%d KB)" % (len(frames), len(never), len(never) * 4))
cls = collections.Counter()
for p, ks in touch.items():
    c = codepg[p]
    tag = ("code" if c > 0.5 else "mixed" if c > 0.05 else "data")
    ks2 = sorted(k if not k.startswith("rt:") else "rt" for k in ks)
    cls[(tag, "+".join(sorted(set(ks2))))] += 1
for (tag, ks), n in sorted(cls.items(), key=lambda x: -x[1]): print("  %-5s read by %-24s %3d pages" % (tag, ks, n))
rtnames = collections.Counter(k for ks in touch.values() for k in ks if k.startswith("rt:"))
print("  runtime readers:", rtnames.most_common(8))
for start in (0, 300):
    sets = [set(pg) for f, pg in frames if f >= start]
    sz = sorted(len(s) for s in sets)
    print("frames >= %d: distinct pages per frame median %d, 99%% %d, max %d" % (start, sz[len(sz)//2], sz[int(len(sz)*.99)], sz[-1]))
    win = [len(set().union(*sets[i:i+60])) for i in range(0, len(sets) - 60, 60)]
    print("   distinct pages per 60-frame window: max %d, median %d" % (max(win), sorted(win)[len(win)//2]))
# LRU
print("LRU page cache (frames >= 300, cache warm from boot):")
for cap in (16, 24, 32, 48, 64, 96):
    lru = collections.OrderedDict(); faults = 0; fault_frames = collections.Counter()
    for f, pgs in frames:
        for pg in pgs:
            if pg in lru: lru.move_to_end(pg)
            else:
                if f >= 300: faults += 1; fault_frames[f] += 1
                lru[pg] = 1
                if len(lru) > cap: lru.popitem(last=False)
    worst = max(fault_frames.values()) if fault_frames else 0
    print("  %3d pages (%4d KB): %5d faults after boot = %.2f MB loaded, frames with faults %d, worst frame %d faults" % (cap, cap*4, faults, faults*PG/1e6, len(fault_frames), worst))
