"""hotset.py ELF TRACE: emulation-stage (stage 0) code lines by entry count;
for thresholds, how many bytes of frequently entered lines each function has,
and in total (runtime text only)."""
import sys, subprocess, bisect, array, collections
elf, trace = sys.argv[1:3]
syms = sorted((int(p[0], 16) & 0x7fffff, int(p[1], 16), p[3]) for p in (l.split() for l in subprocess.run(
    ["/opt/libdragon/bin/mips64-elf-nm", "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines()) if len(p) == 4 and p[2] in "tT")
st = [s[0] for s in syms]
t = array.array("I"); t.frombytes(open(trace, "rb").read())
cnt = collections.Counter(w & 0x7fffe0 for w in t if (w & 3) == 0)
mx = max(cnt.values())
for th in (0.001, 0.0001):
    by = collections.Counter(); mod = 0
    for a, c in cnt.items():
        if c < th * mx: continue
        i = bisect.bisect_right(st, a) - 1
        if i >= 0 and a < syms[i][0] + syms[i][1]: by[syms[i][2]] += 32
        else: mod += 32
    tot = sum(by.values())
    print("lines entered >= %g of the hottest: runtime %d bytes (+ %d bytes of heap code)" % (th, tot, mod))
    print("   " + ", ".join("%s %d" % (n, b) for n, b in by.most_common(16)))
