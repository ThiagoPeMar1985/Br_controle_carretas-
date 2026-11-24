import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_REG_RIP

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
text = None
for i in range(nsec):
    off = opt + 0xF0 + i * 0x28
    name = data[off:off+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    if name == ".text":
        text = (vaddr, vsize, raddr, rsize)
tvaddr, tvsize, traddr, trsize = text

def off_of(va):
    rva = va - BASE - tvaddr
    return traddr + rva if 0 <= rva < trsize else None

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
target = int(sys.argv[1], 16)
cands = set()
for line in open(sys.argv[2]):
    line = line.strip()
    if line.startswith("0x"):
        cands.add(int(line.split(":")[0], 16))
for va in sorted(cands):
    off = off_of(va)
    if off is None: continue
    for insn in md.disasm(data[off:off+16], va, count=1):
        for op in insn.operands:
            if op.type == 3 and op.mem.base == X86_REG_RIP and va + insn.size + op.mem.disp == target:
                print("0x{:X}:  {:7s} {}".format(insn.address, insn.mnemonic, insn.op_str))
