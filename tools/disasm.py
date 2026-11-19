import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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

def rva2off(rva):
    for name, vaddr, vsize, raddr, rsize in secs:
        if vaddr <= rva < vaddr + vsize:
            return raddr + (rva - vaddr)
    return None

def disasm(va, count):
    rva = va - BASE
    off = rva2off(rva)
    if off is None:
        print("RVA 0x{:X} nao mapeado".format(rva)); return
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    code = data[off:off + count * 16]
    n = 0
    for insn in md.disasm(code, va):
        print("0x{:X}:  {:7s} {}".format(insn.address, insn.mnemonic, insn.op_str))
        n += 1
        if n >= count: break

if __name__ == "__main__":
    va = int(sys.argv[1], 16)
    cnt = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    disasm(va, cnt)
