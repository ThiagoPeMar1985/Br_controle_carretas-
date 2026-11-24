import struct, sys
EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
secs = []
for i in range(nsec):
    off = opt + 0xF0 + i * 0x28
    name = data[off:off+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))
def bytes_at(va, n):
    rva = va - BASE
    for nm, v, vs, r, rs in secs:
        if v <= rva < v + vs:
            return data[r + (rva - v): r + (rva - v) + n]
    return None
BS = chr(92)
n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
for spec in sys.argv[2:]:
    b = bytes_at(int(spec, 16), n)
    out = "".join(BS + "x%02x" % c for c in b) if b else "N/A"
    print(spec, out)
