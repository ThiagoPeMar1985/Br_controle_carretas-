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
md = Cs(CS_ARCH_X86, CS_MODE_64)
# scan for rip-relative instructions whose target == target
# rip-relative: modrm mod=00 rm=101 -> byte pattern: after opcode, modrm & 0xC7 == 0x05
i = 0
end = trsize - 16
while i < end:
    # crude: check every position for disp32 that resolves to target when treated as rip-rel
    disp = struct.unpack_from("<i", code, i)[0]
    # instruction end = position of disp + 4; rip = next insn
    # we don't know insn start; try: assume disp at i, insn ends at i+4
    rip = BASE + tvaddr + i + 4
    if rip + disp == target:
        # disasm a window ending past i to find the insn
        start = max(0, i - 8)
        va0 = BASE + tvaddr + start
        for insn in md.disasm(code[start:i+8], va0):
            if insn.address <= BASE + tvaddr + i < insn.address + insn.size:
                print("0x{:X}:  {:7s} {}".format(insn.address, insn.mnemonic, insn.op_str))
                break
    i += 1
