import struct

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
secs = []
for i in range(nsec):
    off = opt + 0xF0 + i * 0x28
    name = data[off:off + 8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))

def off2va(i):
    for name, vaddr, vsize, raddr, rsize in secs:
        if raddr <= i < raddr + rsize:
            return BASE + vaddr + (i - raddr)
    return None

def callers_of(target):
    hits = []
    start = 0
    while True:
        i = data.find(b"\xE8", start)
        if i < 0:
            break
        rel = struct.unpack_from("<i", data, i + 1)[0]
        tgt = BASE + i + 5 + rel  # va da proxima instrucao + rel
        if tgt == target:
            va = off2va(i)
            if va:
                hits.append(va)
        start = i + 1
    return hits

REG = 0x1412B7A20
UNREG = 0x1412B8190
reg_callers = callers_of(REG)
unreg_callers = callers_of(UNREG)
print("REGISTRAR 0x%X: %d call sites" % (REG, len(reg_callers)))
for c in reg_callers:
    print("  0x%X" % c)
print("DESREGISTRAR 0x%X: %d call sites" % (UNREG, len(unreg_callers)))
for c in unreg_callers:
    print("  0x%X" % c)
