"""fnspan.py ELF TRACE FUNC...: for each FUNC, bytes of its 32-byte lines entered
in the emulation stage at least TH of the hottest line, and the offset of the
last such line (how far into the function the hot code reaches)."""
import sys, subprocess, array, collections
elf, trace, funcs = sys.argv[1], sys.argv[2], sys.argv[3:]
sy = {p[3]: (int(p[0], 16) & 0x7fffff, int(p[1], 16)) for p in (l.split() for l in subprocess.run(
    ["/opt/libdragon/bin/mips64-elf-nm", "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines()) if len(p) == 4}
t = array.array("I"); t.frombytes(open(trace, "rb").read())
cnt = collections.Counter(w & 0x7fffe0 for w in t if (w & 3) == 0)
mx = max(cnt.values())
for f in funcs:
    lo, sz = sy[f]
    for th in (0.001, 0.0001):
        hot = [a - lo for a in range(lo & ~31, lo + sz, 32) if cnt.get(a, 0) >= th * mx]
        print("%-22s %6d B  >=%-6g: %5d B hot, last at offset %5d" % (f, sz, th, 32 * len(hot), max(hot) if hot else 0))
