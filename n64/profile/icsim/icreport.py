#!/usr/bin/env python3
"""icreport.py ICPROF ELF NM [FRAMES]: I-cache misses by function and stage,
and the function pairs that evict each other (ares icprof patch output)."""
import sys, subprocess, bisect, collections
icp, elf, nm = sys.argv[1:4]; frames = int(sys.argv[4]) if len(sys.argv) > 4 else 60
syms = sorted((int(p[0],16), int(p[1],16), p[3]) for p in (l.split() for l in subprocess.run([nm,"-S","--defined-only",elf],capture_output=True,text=True).stdout.splitlines()) if len(p)==4 and p[2] in "tTwW")
starts=[s[0] for s in syms]
text_hi=max(a+z for a,z,_ in syms)
def fn(paddr):
    v=paddr|0x80000000
    i=bisect.bisect_right(starts,v)-1
    if i>=0 and v<syms[i][0]+syms[i][1]: return syms[i][2]
    return "<code-cache module>" if v>=text_hi else "<other>"
names={"0":"emulation","1":"PPU","2":"present","3":"?"}
per=collections.defaultdict(collections.Counter); tot=collections.Counter(); lines=collections.defaultdict(set)
pairs=collections.Counter()
for l in open(icp):
    p=l.split()
    if p[0]=="LINE":
        a=int(p[1],16); f=fn(a); per[p[2]][f]+=int(p[3]); tot[p[2]]+=int(p[3]); lines[(p[2],f)].add(a)
    elif p[0]=="EVICT":
        pairs[(fn(int(p[1],16)),fn(int(p[2],16)))]+=int(p[3])
for s in sorted(per):
    print(f"== stage {names[s]}: {tot[s]/frames:.0f} misses/frame")
    for f,n in per[s].most_common(15):
        print(f"   {n/frames:8.0f}/frame {100*n/tot[s]:5.1f}%  {len(lines[(s,f)]):4d} lines  {f}")
print("== top evictions (evicted <- by), misses/frame")
for (a,b),n in pairs.most_common(20):
    print(f"   {n/frames:7.0f}  {a}  <-  {b}")
