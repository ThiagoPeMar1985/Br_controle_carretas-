import sys, struct, subprocess, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
for i in range(nsec):
    off = opt + 0xF0 + i * 0x28
    name = data[off:off+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    if name == ".text": tvaddr, traddr, trsize = vaddr, raddr, rsize
md = Cs(CS_ARCH_X86, CS_MODE_64)

def disasm_at(va, count):
    rva = va - BASE - tvaddr
    off = traddr + rva
    out = []
    for insn in md.disasm(data[off:off+count*16], va):
        out.append((insn.address, insn.mnemonic, insn.op_str))
        if len(out) >= count: break
    return out

# read verified refs list
refs = []
for line in open(r"C:\Users\PC\AppData\Local\Temp\devin.exe-overflows\shell-dc812c-b274c20e43dad917\mgrb_verified.txt")
    m = re.match(r"\s*\d+\|(0x[0-9A-Fa-f]+):", line)
    if m: refs.append(int(m.group(1),16))
refs = sorted(set(refs))
print("total refs:", len(refs))
# for each ref, disasm 30 insns forward; flag if any insn touches +0x740/+0x748/+0x750 on a reg
for va in refs:
    insns = disasm_at(va, 30)
    for a, mn, ops in insns:
        if re.search(r"\+ 0x74[08]", ops) or re.search(r"\+ 0x750", ops):
            print("0x{:X} -> 0x{:X}: {} {}".format(va, a, mn, ops))
            break
