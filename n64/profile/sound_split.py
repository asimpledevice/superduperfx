#!/usr/bin/env python3
"""The sound side of a PC-sample profile (PROFILE=1), CPU phase only: SPC
translated code, the 65816 JIT's code and the C functions by name.

    n64/profile/sound_split.py build/snesrecomp_interp.elf isviewer.log
"""
import sys, subprocess, bisect, collections, re
elf, log = sys.argv[1], sys.argv[2]
syms=[]; bufs={}
for l in subprocess.run(["/opt/libdragon/bin/mips64-elf-nm","-n","-S","--defined-only",elf],capture_output=True,text=True).stdout.splitlines():
    p=l.split()
    if len(p)==4:
        a=int(p[0],16)&0xffffffff; sz=int(p[1],16)
        if p[2] in "tTwW": syms.append((a,p[3]))
        if p[3] in ("s_code","s_jit_code"): bufs[p[3]]=(a,sz)
syms.sort(); addrs=[s[0] for s in syms]
mods=[]
for l in open(log,errors="replace"):
    if l.startswith("PROFMOD "):
        p=l.split(); mods.append((int(p[1],16)&0xffffffff,int(p[2]),p[3]))
mods.sort(); ma=[m[0] for m in mods]
def name(pc):
    for k,(a,sz) in bufs.items():
        if a<=pc<a+sz: return "SPC translated code" if k=="s_code" else "65816 translated code"
    j=bisect.bisect_right(ma,pc)-1
    if j>=0 and pc<mods[j][0]+mods[j][1]: return "cc:"+mods[j][2]
    i=bisect.bisect_right(addrs,pc)-1
    return syms[i][1] if i>=0 else "?"
c=collections.Counter(); tot=0; phase=collections.Counter()
for l in open(log,errors="replace"):
    if l.startswith("PROF ") and not l.startswith("PROF_"):
        p=l.split(); ph=int(p[2]) if len(p)>2 else 0
        phase[ph]+=1
        if ph!=0: continue
        c[name(int(p[1],16))]+=1; tot+=1
print("samples by phase",dict(phase),"CPU phase",tot)
spc=re.compile(r"^(apu_|spc_|dsp_|Spc|snesdsp|rspdsp|apu|RtlApu|rtl_sync_apu|rtl_apu)")
grp=collections.Counter()
for k,n in c.items():
    if k=="SPC translated code" or spc.match(k): grp[k]+=n
s=sum(grp.values())
print("sound-side total: %d samples = %.1f%% of the CPU phase"%(s,100*s/tot))
for k,n in grp.most_common(22): print("  %5.1f%% of sound  %5.2f%% of CPU phase  %s"%(100*n/s,100*n/tot,k))
print("top others:")
for k,n in [x for x in c.most_common(40) if x[0] not in grp][:10]: print("  %5.2f%%  %s"%(100*n/tot,k))
