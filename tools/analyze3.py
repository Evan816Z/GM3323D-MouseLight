# Align each event's stream to the initial stream, count which offsets change over time.
import re
from collections import defaultdict

path = r"C:\Users\tongz\Desktop\mouse\tools\mouse_dump\dump_20261018_091008.txt"
def hx(s): return [int(x,16) for x in s.strip().split()]

snap={}; timeline=[]
with open(path) as f: lines=f.read().splitlines()
i=0
while i<len(lines):
    ln=lines[i]
    m=re.match(r"\s*\[initial\] rid=0x([0-9A-Fa-f]+):\s*(.*)",ln)
    if m: snap[int(m.group(1),16)]=hx(m.group(2)); i+=1; continue
    m=re.match(r"\s*\[T\+([0-9.]+)s\] rid=0x([0-9A-Fa-f]+) CHANGED:",ln)
    if m:
        t=float(m.group(1)); rid=int(m.group(2),16); old=new=None; i+=1
        while i<len(lines):
            m2=re.match(r"\s*old:\s*(.*)",lines[i])
            if m2: old=hx(m2.group(1)); i+=1; continue
            m2=re.match(r"\s*new:\s*(.*)",lines[i])
            if m2: new=hx(m2.group(1)); i+=1; continue
            break
        snap[rid]=new; timeline.append((t,dict(snap))); continue
    i+=1

RIDS=[r for r in sorted(snap.keys()) if 4<=r<=46]
def build(sn):
    parts=[sn[r][:] for r in RIDS if r in sn]
    stream=parts[0][:]
    for p in parts[1:]:
        best=0
        for ov in range(1,17):
            if stream[-ov:]==p[:ov]: best=ov
        stream += p[best:] if best else [None]*4+p[:]
    return stream

base=build(timeline[0][1])
N=len(base)
# sample some unique anchors from base
def align(stream):
    """find shift of stream relative to base maximizing matches"""
    best=( -1, 0)
    for sh in range(-300,301):
        m=0; tot=0
        # compare a sample window
        for j in range(0,N,7):
            k=j+sh
            if 0<=k<len(stream) and base[j] is not None and stream[k] is not None:
                tot+=1
                if base[j]==stream[k]: m+=1
        if tot>200 and m/tot>best[0]: best=(m/tot, sh)
    return best  # frac, shift (stream index = base index + shift)

results=[]  # (t, shift, frac)
for t,sn in timeline:
    st=build(sn)
    frac,sh=align(st)
    results.append((t,sh,frac,st))

# print shift over time
prev=None
for t,sh,frac,st in results:
    if prev is None or sh!=prev:
        print(f"T+{t:7.1f}s shift={sh:+4d} match={frac:.3f}")
        prev=sh

# For stable periods (same shift), diff base vs stream and record changed offsets
changes=defaultdict(list)  # offset -> list of (t, oldval, newval)
for t,sh,frac,st in results:
    if frac<0.9: continue
    for j in range(N):
        k=j+sh
        if 0<=k<len(st) and base[j] is not None and st[k] is not None and base[j]!=st[k]:
            changes[j].append((t,base[j],st[k]))

print()
print("offsets with changes:", len(changes))
for j in sorted(changes):
    evs=changes[j]
    if len(evs)<=40:  # skip noisy offsets
        vals=set((o,n) for _,o,n in evs)
        print(f"off {j:5d} ({j//132:>3}+{j%132:3}) n={len(evs):3d} vals={sorted(vals)[:8]}")
