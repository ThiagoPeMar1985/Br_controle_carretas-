import sys, struct
EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE,"rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
secs=[]
for i in range(nsec):
    off = opt + 0xF0 + i*0x28
    name = data[off:off+8].rstrip(b"\0").decode()
    vsize,vaddr,rsize,raddr = struct.unpack_from("<IIII", data, off+8)
    secs.append((name,vaddr,vsize,raddr,rsize))
def rva2off(rva):
    for name,vaddr,vsize,raddr,rsize in secs:
        if vaddr <= rva < vaddr+vsize: return raddr+(rva-vaddr)
va = int(sys.argv[1],16)
count = int(sys.argv[2]) if len(sys.argv)>2 else 30
off = rva2off(va-BASE)
if off is None: print("nao mapeado"); sys.exit()
for i in range(count):
    e = struct.unpack_from("<Q", data, off+i*8)[0]
    tag = "exe" if 0x140000000 <= e < 0x142000000 else ("rdata" if 0x142000000<=e<0x144000000 else "out")
    print("+0x{:03X}: 0x{:X}  ({})".format(i*8, e, tag))
