# batchtrace.py ELF TRACE NM OUT: rewrite an ares icprof trace so each frame's
# APU-side lines (spc.o/dsp.o/apu.o) run after its 65816-side lines: replay OUT
# with icsim for the ceiling of running the SPC700 as one batch per frame.
# Run from n64/.
import sys, subprocess, array, bisect
elf, trace, nm, out = sys.argv[1:5]
apu=set()
for o in ['build/runner/snes/spc.o','build/runner/snes/dsp.o','build/runner/snes/apu.o']:
    for l in subprocess.run([nm,'--defined-only',o],capture_output=True,text=True).stdout.split('\n'):
        p=l.split()
        if len(p)==3 and p[1] in 'tT': apu.add(p[2])
rng=[]
for l in subprocess.run([nm,'-S','--defined-only',elf],capture_output=True,text=True).stdout.split('\n'):
    p=l.split()
    if len(p)==4 and p[2] in 'tT' and p[3] in apu:
        a=int(p[0],16)&0x1fffffff; rng.append((a,a+int(p[1],16)))
rng.sort(); los=[r[0] for r in rng]; c={}
def isapu(a):
    if a in c: return c[a]
    i=bisect.bisect_right(los,a)-1
    r= i>=0 and a<rng[i][1]; c[a]=r; return r
t=array.array('I'); t.frombytes(open(trace,'rb').read())
o=array.array('I'); cpu=[]; ap=[]
def flush():
    o.extend(cpu); o.extend(ap); cpu.clear(); ap.clear()
for w in t:
    if w&3:
        flush(); o.append(w)
    else:
        (ap if isapu((w&~31)&0x1fffffff) else cpu).append(w)
flush(); o.tofile(open(out,'wb'))
