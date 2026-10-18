# Find config-signature windows (05 0d 01 ...) anywhere in any rid snapshot across the timeline.
import re
path = r"C:\Users\tongz\Desktop\mouse\tools\mouse_dump\dump_20261018_091008.txt"
def hx(s): return [int(x,16) for x in s.strip().split()]

snaps=[]  # (t, rid, bytes)  -- each recorded state (initial + every new)
with open(path) as f: lines=f.read().splitlines()
i=0
while i<len(lines):
    ln=lines[i]
    m=re.match(r"\s*\[initial\] rid=0x([0-9A-Fa-f]+):\s*(.*)",ln)
    if m:
        snaps.append((-1.0,int(m.group(1),16),hx(m.group(2)))); i+=1; continue
    m=re.match(r"\s*\[T\+([0-9.]+)s\] rid=0x([0-9A-Fa-f]+) CHANGED:",ln)
    if m:
        t=float(m.group(1)); rid=int(m.group(2),16); old=new=None; i+=1
        while i<len(lines):
            m2=re.match(r"\s*old:\s*(.*)",lines[i])
            if m2: old=hx(m2.group(1)); i+=1; continue
            m2=re.match(r"\s*new:\s*(.*)",lines[i])
            if m2: new=hx(m2.group(1)); i+=1; continue
            break
        if old: snaps.append((t,rid,old))
        if new: snaps.append((t,rid,new))
        continue
    i+=1

def find_sig(b):
    out=[]
    for j in range(len(b)-20):
        if b[j]==0x05 and b[j+1]==0x0d and b[j+2]==0x01:
            out.append(j)
    return out

hits=[]
for t,rid,b in snaps:
    for j in find_sig(b):
        hits.append((t,rid,j,b))

print(f"config-signature hits: {len(hits)}")
# dedupe by the 40-byte content
seen={}
order=[]
for t,rid,j,b in hits:
    seg=tuple(b[j:j+60])
    if seg not in seen:
        seen[seg]=(t,rid,j); order.append(seg)

print(f"distinct configs: {len(order)}")
for seg in order:
    t,rid,j=seen[seg]
    print(f"\nT+{t:6.1f}s rid=0x{rid:02X} off={j}")
    print("  ", " ".join(f"{x:02x}" for x in seg))
