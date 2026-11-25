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

text = next(s for s in secs if s[0] == ".text")
to, tv, tsz = text[3], text[1], text[2]
code = data[to:to+tsz]

va = int(sys.argv[1], 16)
size = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x800
off = (va - BASE) - tv
chunk = code[off:off+size]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for insn in md.disasm(chunk, va):
    ops = insn.op_str
    if insn.mnemonic in ("mov","add","sub","inc","dec","and","or","xor") and \
       ("+ 0x28]" in ops or "+ 0x20]" in ops or "+ 0x10]" in ops) and \
       ("ptr [" in ops and ops.index("ptr [") < (ops.index(",") if "," in ops else 9999)):
        print("0x{:X}:  {} {}".format(insn.address, insn.mnemonic, ops))
