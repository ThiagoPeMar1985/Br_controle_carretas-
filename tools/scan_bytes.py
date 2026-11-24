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
t=[s for s in secs if s[0]==".text"][0]
_,tvaddr,tvsize,traddr,trsize=t

code = data[traddr:traddr+trsize]
pat = struct.pack("<Q", int(sys.argv[1], 16))
md = Cs(CS_ARCH_X86, CS_MODE_64)
i = 0
while True:
    j = code.find(pat, i)
    if j < 0: break
    i = j + 1
    start = max(0, j - 12)
    va0 = BASE + tvaddr + start
    for insn in md.disasm(code[start:j+16], va0):
        if insn.address <= BASE + tvaddr + j < insn.address + insn.size:
            print("0x{:X}:  {:7s} {}".format(insn.address, insn.mnemonic, insn.op_str))
            break
