import struct, sys

DMP = r"C:\Users\PC\AppData\Local\CrashDumps\eurotrucks2.exe.20372.dmp"
data = open(DMP, "rb").read()

assert data[:4] == b"MDMP", "nao e minidump"
nstreams = struct.unpack_from("<I", data, 8)[0]
dirrva = struct.unpack_from("<I", data, 12)[0]

streams = {}
for i in range(nstreams):
    st, sz, rva = struct.unpack_from("<III", data, dirrva + i * 12)
    streams.setdefault(st, (sz, rva))

# modulo principal
exe_base = None
sz, rva = streams[4]
n = struct.unpack_from("<I", data, rva)[0]
for i in range(n):
    off = rva + 4 + i * 108
    base, sizeimg = struct.unpack_from("<QI", data, off)
    name_rva = struct.unpack_from("<I", data, off + 20)[0]
    ln = struct.unpack_from("<I", data, name_rva)[0]
    name = data[name_rva + 4:name_rva + 4 + ln].decode("utf-16-le", "ignore")
    if "eurotrucks2.exe" in name.lower():
        exe_base = base
        exe_size = sizeimg
print("exe base=0x{:X} size=0x{:X}".format(exe_base, exe_size))

# memoria: Memory64ListStream (9) ou MemoryListStream (5)
ranges = []
if 9 in streams:
    sz, rva = streams[9]
    nranges = struct.unpack_from("<Q", data, rva)[0]
    base_rva = struct.unpack_from("<Q", data, rva + 8)[0]
    cum = base_rva
    for i in range(nranges):
        start, dsize = struct.unpack_from("<QQ", data, rva + 16 + i * 16)
        ranges.append((start, dsize, cum))
        cum += dsize
elif 5 in streams:
    sz, rva = streams[5]
    nranges = struct.unpack_from("<I", data, rva)[0]
    cum = rva + 4
    for i in range(nranges):
        off = rva + 4 + i * 16
        start = struct.unpack_from("<Q", data, off)[0]
        dsize, drva = struct.unpack_from("<II", data, off + 8)
        ranges.append((start, dsize, drva))
print("ranges de memoria:", nranges, "total:", sum(r[1] for r in ranges))

import bisect
starts = [r[0] for r in ranges]

def read_va(addr, size):
    out = bytearray()
    while size > 0:
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return None
        rstart, rsize, roff = ranges[i]
        if addr >= rstart + rsize:
            return None
        take = min(size, rstart + rsize - addr)
        out += data[roff + (addr - rstart): roff + (addr - rstart) + take]
        addr += take
        size -= take
    return bytes(out)

def u64(addr):
    b = read_va(addr, 8)
    return struct.unpack("<Q", b)[0] if b else None

# 1) disconnect impl = exe+0x789C20 (vtbl[99] de toda classe carreta)
DISC = exe_base + 0x789C20
print("disconnect impl @0x{:X} -> 0x{:X}".format(DISC, u64(DISC) or 0))

# 2) vtables das classes de veiculo/carreta (medidas no lane dump da sessao)
#    own_vtbl exe+0x24141E0 / +0x2414CA0 / +0x24106A8 / +0x24125B0
VT_OFFS = (0x24141E0, 0x2414CA0, 0x24106A8, 0x24125B0)
vt_vals = [exe_base + o for o in VT_OFFS]
print("procurando objetos com vtables:", [hex(v) for v in vt_vals])

# 3) varrer TODA a memoria pelos ponteiros de vtable -> objetos veiculo/carreta
trailers = []
for vt in vt_vals:
    pat = struct.pack("<Q", vt)
    for rstart, rsize, roff in ranges:
        blob = data[roff:roff + rsize]
        pos = 0
        while True:
            i = blob.find(pat, pos)
            if i < 0:
                break
            va = rstart + i
            if va % 8 == 0:
                trailers.append(va)
            pos = i + 8
trailers.sort()
print("objetos com essas vtables:", len(trailers))
for t in trailers[:40]:
    parent = u64(t + 0x1050)
    slave = u64(t + 0x1058)
    rec = u64(t + 0x1158)
    p1168 = read_va(t + 0x1168, 1)
    print("  obj 0x{:X} parent=0x{:X} slave=0x{:X} rec=0x{:X} +1168={}".format(
        t, parent or 0, slave or 0, rec or 0, p1168.hex() if p1168 else "?"))

# 4) gerenciador de estacionados
mgr = u64(exe_base + 0x3715010)
print("parked mgr global (exe+0x3715010) = 0x{:X}".format(mgr or 0))
if mgr:
    n = u64(mgr + 0x748)
    arr = u64(mgr + 0x740)
    print("  lista: arr=0x{:X} count={}".format(arr or 0, n or 0))
    if arr and n and n < 0x100:
        for k in range(n):
            r = u64(arr + k * 8)
            own = u64(r + 0x178) if r else None
            print("    rec[{}] 0x{:X} -> trl 0x{:X}".format(k, r or 0, own or 0))
