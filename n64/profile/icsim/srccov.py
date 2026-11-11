"""srccov.py FUNC (ELF TRACE)...: for FUNC, which of its source lines (the
outermost frame inside FUNC) executed in each trace, and how often its code
lines were entered. Prints per-source-line entry counts and a summary."""
import sys, subprocess, collections, re, array
func = sys.argv[1]; pairs = list(zip(sys.argv[2::2], sys.argv[3::2]))
OBJ = "/opt/libdragon/bin/mips64-elf-objdump"; A2L = "/opt/libdragon/bin/mips64-elf-addr2line"; NM = "/opt/libdragon/bin/mips64-elf-nm"
hot = collections.Counter(); allsrc = set(); bytes_by_src = collections.Counter()
for elf, trace in pairs:
    lo = sz = None
    for l in subprocess.run([NM, "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 4 and p[3] == func: lo, sz = int(p[0], 16), int(p[1], 16)
    entries = collections.Counter()
    t = array.array("I"); t.frombytes(open(trace, "rb").read())
    plo, phi = lo & 0x7fffff, (lo + sz) & 0x7fffff
    for w in t:
        a = w & 0x7fffe0
        if plo <= a < phi: entries[a] += 1
    addrs = list(range(lo, lo + sz, 4))
    out = subprocess.run([A2L, "-i", "-f", "-a", "-e", elf], input="\n".join("%x" % a for a in addrs), capture_output=True, text=True).stdout
    blocks = out.strip().split("\n0x")
    for a, b in zip(addrs, blocks):
        ls = b.split("\n")[1:]
        frames = list(zip(ls[0::2], ls[1::2]))
        src = None
        for f, loc in frames:
            if f == func:
                m = re.search(r":(\d+)", loc); src = int(m.group(1)) if m else None
        if src is None: continue
        allsrc.add(src); bytes_by_src[src] += 4
        hot[src] += entries.get((a & 0x7fffff) & ~31, 0)
exe = {s for s in allsrc if hot[s]}
print("%s: %d source lines with code, %d executed; code bytes: executed %d, never %d" % (
    func, len(allsrc), len(exe), sum(bytes_by_src[s] for s in exe) // len(pairs), sum(bytes_by_src[s] for s in allsrc - exe) // len(pairs)))
for s in sorted(allsrc):
    print("%6d %8d %s" % (s, hot[s], "X" if hot[s] else "."))
