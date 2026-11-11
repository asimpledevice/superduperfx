# switches.py ELF TRACE NM: count 65816-side <-> APU-side (spc.o/dsp.o/apu.o)
# alternations in the emulation stage of an ares icprof trace. Run from n64/.
import sys, subprocess, array, bisect
elf, trace, nm = sys.argv[1], sys.argv[2], sys.argv[3]
apu = set()
for o in ['build/runner/snes/spc.o','build/runner/snes/dsp.o','build/runner/snes/apu.o']:
    for l in subprocess.run([nm,'--defined-only',o],capture_output=True,text=True).stdout.split('\n'):
        p=l.split()
        if len(p)==3 and p[1] in 'tT': apu.add(p[2])
rng=[]
for l in subprocess.run([nm,'-S','--defined-only',elf],capture_output=True,text=True).stdout.split('\n'):
    p=l.split()
    if len(p)==4 and p[2] in 'tT' and p[3] in apu:
        a=int(p[0],16)&0x1fffffff; rng.append((a,a+int(p[1],16)))
rng.sort(); los=[r[0] for r in rng]
def isapu(a):
    i=bisect.bisect_right(los,a)-1
    return i>=0 and a<rng[i][1]
t=array.array('I'); t.frombytes(open(trace,'rb').read())
sw=0; prev=None; n=0; apun=0
for w in t:
    if w&3: continue
    a=(w&~31)&0x1fffffff
    c=isapu(a); n+=1; apun+=c
    if prev is not None and c!=prev: sw+=1
    prev=c
print(f"stage0 lines {n} apu lines {apun} switches {sw} lines/switch {n/max(sw,1):.0f}")
