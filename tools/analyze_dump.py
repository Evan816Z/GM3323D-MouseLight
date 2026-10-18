# Parse mouse_dump WATCH output; track rid snapshots over time; locate DPI/poll-rate fields.
import re, sys

path = r"C:\Users\tongz\Desktop\mouse\tools\mouse_dump\dump_20261018_091008.txt"

def hx(s):
    return [int(x, 16) for x in s.strip().split()]

snap = {}          # rid -> latest bytes
timeline = []      # (t, rid, old, new)

with open(path) as f:
    lines = f.read().splitlines()

i = 0
t = -1
while i < len(lines):
    ln = lines[i]
    m = re.match(r"\s*\[initial\] rid=0x([0-9A-Fa-f]+):\s*(.*)", ln)
    if m:
        rid = int(m.group(1), 16); snap[rid] = hx(m.group(2)); i += 1; continue
    m = re.match(r"\s*\[T\+([0-9.]+)s\] rid=0x([0-9A-Fa-f]+) CHANGED:", ln)
    if m:
        t = float(m.group(1)); rid = int(m.group(2), 16)
        old = new = None
        i += 1
        while i < len(lines):
            m2 = re.match(r"\s*old:\s*(.*)", lines[i])
            if m2: old = hx(m2.group(1)); i += 1; continue
            m2 = re.match(r"\s*new:\s*(.*)", lines[i])
            if m2: new = hx(m2.group(1)); i += 1; continue
            break
        snap[rid] = new
        timeline.append((t, rid, old, new))
        continue
    i += 1

print("rids:", sorted(snap.keys()))
print("change events:", len(timeline))
from collections import Counter
print("events per rid:", Counter(r for _, r, _, _ in timeline))
print()
# print timeline condensed: per event, which byte offsets in rid=0x04 differ
for t, rid, old, new in timeline:
    if old and new and len(old) == len(new):
        diffs = [j for j in range(len(old)) if old[j] != new[j]]
        if diffs:
            rng = (min(diffs), max(diffs))
        else:
            rng = None
        print(f"T+{t:6.1f}s rid=0x{rid:02X} len={len(new)} ndiff={len(diffs)} range={rng}")
    else:
        print(f"T+{t:6.1f}s rid=0x{rid:02X} len={len(new) if new else '?'} (no old)")
