import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE,"rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
for i in range(nsec):
    off = opt + 0xF0 + i*0x28
    name = data[off:off+8].rstrip(b"\0").decode()
    vsize,vaddr,rsize,raddr = struct.unpack_from("<IIII", data, off+8)
    if name==".text": tvaddr,traddr,trsize=vaddr,raddr,rsize
code = data[traddr:traddr+trsize]
target = int(sys.argv[1],16)
# E8 rel32 call
pat_off = 0
md = Cs(CS_ARCH_X86, CS_MODE_64)
i=0
while True:
    j = code.find(b"\xe8", i)
    if j<0: break
    i=j+1
    rel = struct.unpack_from("<i", code, j+1)[0]
    va = BASE+tvaddr+j
    if va+5+rel==target:
        print("call site 0x{:X}".format(va))
