#!/usr/bin/env python3
"""layout_eval.py ELF TRACE ORDER [--colour BYTES] [--nm NM] [--icsim BIN]:
estimate a rule-based layout without relinking (ORDER - keeps the ELF
layout and only colours the heap). ORDER lists function names,
one per line ('@0xOFF' pads to that offset in the text, '#' comments);
listed functions are laid out first from the text start (16-byte aligned),
the remaining ELF functions follow in their current order. Code outside the
ELF text (code-cache modules on the heap) keeps its order but, with
--colour B, is packed only into offsets [B, 16K) of each 16 KB window, as a
coloured module allocator would. Replays TRACE through icsim with the remap."""
import sys, subprocess, argparse, os, tempfile, array
ap = argparse.ArgumentParser(); ap.add_argument("elf"); ap.add_argument("trace"); ap.add_argument("order")
ap.add_argument("--colour", type=lambda s: int(s, 0), default=0); ap.add_argument("--nm", default="mips64-elf-nm")
ap.add_argument("--icsim", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "icsim"))
ap.add_argument("--text", type=lambda s: int(s, 0), default=0x1000)
a = ap.parse_args()
fns = []
for l in subprocess.run([a.nm, "-S", "-n", "--defined-only", a.elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in "tTwW":
        ad = int(p[0], 16) & 0x7fffff; sz = int(p[1], 16)
        if sz and ad >= a.text: fns.append((ad, sz, p[3]))
byname = {}
for f in fns: byname.setdefault(f[2], f)
tlo = min(f[0] for f in fns); thi = max(f[0] + f[1] for f in fns)
pos = tlo; placed = set(); out = []
keep = a.order == "-"
for l in ([] if keep else open(a.order)):
    l = l.split("#")[0].strip()
    if not l: continue
    if l.startswith("@"):
        pos = max(pos, tlo + int(l[1:], 0)); continue
    f = byname.get(l)
    if not f or l in placed: continue
    pos = (pos + 15) & ~15; out.append((f[0], f[0] + f[1], pos)); pos += f[1]; placed.add(l)
for f in ([] if keep else fns):
    if f[2] in placed: continue
    pos = (pos + 15) & ~15; out.append((f[0], f[0] + f[1], pos)); pos += f[1]; placed.add(f[2])
if a.colour:
    t = array.array("I"); t.frombytes(open(a.trace, "rb").read())
    heap = sorted({(w & ~31) & 0x7fffff for w in t if not (tlo <= ((w & ~31) & 0x7fffff) < thi + 64)})
    span = 16384 - a.colour; base = (thi + 0x100000) & ~16383; v = 0; prev = None
    for h in heap:
        v += 32 if prev is None or h - prev > 4096 else h - prev   # keep gaps inside a module
        prev = h; win, off = divmod(v, span)
        out.append((h, h + 32, base + win * 16384 + a.colour + off - (off % 32)))
with tempfile.NamedTemporaryFile("w", delete=False) as r:
    for o in out: r.write("%x %x %x\n" % o)
res = subprocess.run([a.icsim, a.trace, r.name], capture_output=True, text=True).stdout
os.unlink(r.name)
print(" ".join(l.split()[2] for l in res.splitlines() if l.startswith("STAGE"))[:200], "| text ends +%#x" % (pos - tlo))
