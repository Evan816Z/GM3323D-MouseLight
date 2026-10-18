# Reconstruct continuous memory stream per timestamp from overlapping rid windows,
# then diff across time to find config bytes (DPI, polling rate).
import re

path = r"C:\Users\tongz\Desktop\mouse\tools\mouse_dump\dump_20261018_091008.txt"

def hx(s): return [int(x,16) for x in s.strip().split()]

snap = {}
timeline = []
with open(path) as f: lines = f.read().splitlines()
i=0
while i < len(lines):
    ln = lines[i]
    m = re.match(r"\s*\[initial\] rid=0x([0-9A-Fa-f]+):\s*(.*)", ln)
    if m:
        snap[int(m.group(1),16)] = hx(m.group(2)); i+=1; continue
    m = re.match(r"\s*\[T\+([0-9.]+)s\] rid=0x([0-9A-Fa-f]+) CHANGED:", ln)
    if m:
        t=float(m.group(1)); rid=int(m.group(2),16); old=new=None; i+=1
        while i < len(lines):
            m2=re.match(r"\s*old:\s*(.*)",lines[i])
            if m2: old=hx(m2.group(1)); i+=1; continue
            m2=re.match(r"\s*new:\s*(.*)",lines[i])
            if m2: new=hx(m2.group(1)); i+=1; continue
            break
        snap[rid]=new; timeline.append((t,dict(snap))); continue
    i+=1

RIDS = [r for r in sorted(snap.keys()) if 4 <= r <= 46]

def build_stream(sn):
    """Chain rid windows 4..46 into one stream; trim overlap using tail/head match."""
    parts = [sn[r][:] for r in RIDS if r in sn]
    if not parts: return None
    stream = parts[0][:]
    for p in parts[1:]:
        # find overlap: suffix of stream == prefix of p (up to 16 bytes)
        best = 0
        for ov in range(1,17):
            if len(stream)>=ov and stream[-ov:]==p[:ov]: best=ov
        stream += p[best:] if best else [None]*4 + p[:]  # unknown gap
    return stream

def find_all(stream, pat):
    res=[]
    n=len(pat)
    for j in range(len(stream)-n+1):
        w=stream[j:j+n]
        if all(a is None or a==b for a,b in zip(w,pat)): res.append(j)
    return res

# candidate encodings of DPI table 500,1200,1600,2400,3200,5100
cands = {
 "u16LE": [0xf4,0x01,0xb0,0x04,0x40,0x06,0x60,0x09,0x80,0x0c,0xec,0x13],
 "u16BE": [0x01,0xf4,0x04,0xb0,0x06,0x40,0x09,0x60,0x0c,0x80,0x13,0xec],
 "u16LE/50":[0x0a,0x00,0x18,0x00,0x20,0x00,0x30,0x00,0x40,0x00,0x66,0x00],
 "u8": [0x0a,0x18,0x20,0x30,0x40,0x66],
 "u8-200":[300%256,(1200-200)%256,(1600-200)%256,(2400-200)%256,(3200-200)%256,(5100-200)%256],
}

streams = {}  # event index -> stream
for k,(t,sn) in enumerate(timeline):
    streams[k] = build_stream(sn)
t0, sn0 = timeline[0]
base = build_stream(sn0)
print("stream len:", len(base))
for name,pat in cands.items():
    hits = find_all(base, pat)
    print(f"initial stream, pattern {name}: hits at {hits[:10]}")

# also search individual dpi values u16LE in initial
for v in (500,1200,1600,2400,3200,5100):
    le=[v&0xff,(v>>8)&0xff]; be=[(v>>8)&0xff,v&0xff]
    print(v, "LE:", find_all(base,le)[:8], "BE:", find_all(base,be)[:8], "u8val/50:", find_all(base,[v//50]), "u8val/100:", find_all(base,[v//100] if v%100==0 else [-1]))
