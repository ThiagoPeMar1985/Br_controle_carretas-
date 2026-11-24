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
        if vaddr <= rva < vaddr+vsize: return raddr+(rva-vaddr),name
    return None,None
va = int(sys.argv[1],16)
count = int(sys.argv[2]) if len(sys.argv)>2 else 8
off,sec = rva2off(va-BASE)
print("VA 0x{:X} em secao {} off 0x{:X}".format(va, sec, off))
for i in range(count):
    e = struct.unpack_from("<Q", data, off+i*8)[0]
    print("+0x{:02X}: 0x{:016X}".format(i*8, e))
