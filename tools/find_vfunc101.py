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
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    secs.append((vaddr, vsize, raddr, rsize))

def off2va(i):
    for vaddr, vsize, raddr, rsize in secs:
        if raddr <= i < raddr + rsize:
            return BASE + vaddr + (i - raddr)
    return None

# acha vtables que contem o disconnect impl (vfunc 99 = 0x140789C20) no slot 99
target = struct.pack("<Q", 0x140789C20)
start = 0
found = 0
while found < 4:
    i = data.find(target, start)
    if i < 0:
        break
    va = off2va(i)
    if va:
        vt = va - 99 * 8  # o slot 99 = offset 0x318
        # le o slot 101 (offset 0x328)
        slot101_off = i + (101 - 99) * 8
        slot101 = struct.unpack_from("<Q", data, slot101_off)[0]
        print("vtable 0x%X: vfunc99=0x140789C20  vfunc101=0x%X" % (vt, slot101))
        found += 1
    start = i + 1
