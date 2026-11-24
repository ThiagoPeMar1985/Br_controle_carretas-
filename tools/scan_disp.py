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
want = int(sys.argv[1], 16)
pat = struct.pack("<I", want)

# find all offsets where disp32 == want inside .text
hits = []
i = 0
while True:
    j = code.find(pat, i)
    if j < 0: break
    hits.append(j)
    i = j + 1

md = Cs(CS_ARCH_X86, CS_MODE_64)
seen = set()
for j in hits:
    # disassemble a window ending around j to find the instruction that owns this disp
    start = max(0, j - 10)
    va0 = BASE + tvaddr + start
    for insn in md.disasm(code[start:j+8], va0):
        if insn.address + insn.size > BASE + tvaddr + j >= insn.address:
            if insn.address not in seen:
                seen.add(insn.address)
                print("0x{:X}:  {:7s} {}".format(insn.address, insn.mnemonic, insn.op_str))
            break
