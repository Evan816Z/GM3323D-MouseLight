# Extract localized diffs (old vs new within same rid window, few bytes changed).
import re
path = r"C:\Users\tongz\Desktop\mouse\tools\mouse_dump\dump_20261018_091008.txt"
def hx(s): return [int(x,16) for x in s.strip().split()]

events=[]
with open(path) as f: lines=f.read().splitlines()
i=0
while i<len(lines):
    ln=lines[i]
    m=re.match(r"\s*\[T\+([0-9.]+)s\] rid=0x([0-9A-Fa-f]+) CHANGED:",ln)
    if m:
        t=float(m.group(1)); rid=int(m.group(2),16); old=new=None; i+=1
        while i<len(lines):
            m2=re.match(r"\s*old:\s*(.*)",lines[i])
            if m2: old=hx(m2.group(1)); i+=1; continue
            m2=re.match(r"\s*new:\s*(.*)",lines[i])
            if m2: new=hx(m2.group(1)); i+=1; continue
            break
        if old and new and len(old)==len(new):
            diffs=[j for j in range(len(old)) if old[j]!=new[j]]
            events.append((t,rid,diffs,old,new))
        continue
    i+=1

print(f"total events: {len(events)}")
print("=== localized diffs (<=12 bytes changed) ===")
for t,rid,diffs,old,new in events:
    if len(diffs)<=12:
        desc="; ".join(f"[{j}] {old[j]:02x}->{new[j]:02x}" for j in diffs)
        print(f"T+{t:6.1f}s rid=0x{rid:02X} {desc}")
