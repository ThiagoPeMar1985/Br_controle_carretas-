import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

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
code = data[traddr:traddr+trsize]
target = int(sys.argv[1], 16)
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

pos = 0
end = trsize
while pos < end:
    va = BASE + tvaddr + pos
    hit = False
    for insn in md.disasm(code[pos:pos+15], va, count=1):
        # check displacement-bearing operands
        for op in insn.operands:
            if op.type == 3:  # MEM
                mem = op.mem
                if mem.base == 41:  # X86_REG_RIP
                    if va + insn.size + mem.disp == target:
                        print("0x{:X}:  {:7s} {}".format(insn.address, insn.mnemonic, insn.op_str))
                        hit = True
        pos += insn.size
        hit = True
        break
    if not hit:
        pos += 1
