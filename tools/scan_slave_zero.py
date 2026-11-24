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

# mov qword ptr [reg+0x1058], 0  ->  REX C7 /0 mod=10 rm=base, disp32=0x1058, imm32=0
hits = []
for rex in (0x48, 0x49, 0x41):
    for rm in range(8):
        modrm = 0x80 | rm
        pat = bytes([rex, 0xC7, modrm, 0x58, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        start = 0
        while True:
            i = data.find(pat, start)
            if i < 0:
                break
            va = off2va(i)
            if va:
                hits.append(va)
            start = i + 1

hits.sort()
print("escritas de 0 em +0x1058 (slave):", len(hits))
for h in hits:
    print("  0x{:X}".format(h))
