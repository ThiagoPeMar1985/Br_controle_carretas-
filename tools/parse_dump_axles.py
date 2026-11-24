import re, struct

path = r"C:\Users\PC\Documents\Euro Truck Simulator 2\br_carretas_memdump.txt"
lines = open(path, encoding="utf-8", errors="replace").read().splitlines()

# rebuild each object's bytes
objs = {}
cur = None
hexline = re.compile(r"^\s*\+0x([0-9A-Fa-f]+):\s+([0-9A-Fa-f ]+)$")
for ln in lines:
    m = re.match(r"^== (\S+)", ln)
    if m:
        cur = m.group(1); objs[cur] = {}
        continue
    hm = hexline.match(ln)
    if hm and cur:
        off = int(hm.group(1), 16)
        bts = bytes(int(x, 16) for x in hm.group(2).split())
        objs[cur][off] = bts

def get(obj, off, size):
    d = objs.get(obj, {})
    for base in sorted(d):
        if base <= off < base + len(d[base]):
            s = off - base
            if s + size <= len(d[base]):
                return d[base][s:s+size]
    return None

def u64(obj, off):
    b = get(obj, off, 8); return struct.unpack("<Q", b)[0] if b else None
def f32(obj, off):
    b = get(obj, off, 4); return round(struct.unpack("<f", b)[0], 4) if b else None
def u8(obj, off):
    b = get(obj, off, 1); return b[0] if b else None

print(f"{'obj':10} {'+6B8 ang':>10} {'+6C0 steer':>14} {'+6C8 lift':>14} {'+4D4 susp':>10} {'+150 wheel':>14}")
for name in objs:
    sd = u64(name, 0x6C0)
    lc = u64(name, 0x6C8)
    print(f"{name:10} {f32(name,0x6B8)!s:>10} {sd and hex(sd) or 'NULL':>14} {lc and hex(lc) or 'NULL':>14} {f32(name,0x4D4)!s:>10} {hex(u64(name,0x150) or 0):>14}")

print()
for name in ("truck", "trailer0", "trailer1", "trailer2"):
    cd = u64(name, 0x200)
    print(f"{name}: +0x200 chassis_data = {cd and hex(cd) or 'NULL'}")
