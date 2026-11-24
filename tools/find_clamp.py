import struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()

pe = struct.unpack_from("<I", data, 0x3C)[0]
sec_off = pe + 0x18 + struct.unpack_from("<H", data, pe + 0x14)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
secs = []
for i in range(nsec):
    o = sec_off + i * 40
    name = data[o:o+8].rstrip(b"\0").decode("ascii","replace")
    vsize, va, rsize, roff = struct.unpack_from("<IIII", data, o+8)
    secs.append((name, va, vsize, roff, rsize))

def va_at(off):
    for n,va,vsz,ro,rs in secs:
        if ro <= off < ro+rs: return BASE + va + (off - ro)
    return None

# find error string
for pat in [b"out of the allowed range", b"allowed range"]:
    idx = data.find(pat)
    print(f"{pat}: off={idx:x} va={va_at(idx):x}" if idx>=0 else f"{pat}: not found")
    str_va = va_at(idx)
    break

# scan .text for rip refs to str_va — robust: scan for any rip operand
tsec = next(s for s in secs if s[0]==".text")
tva, tvsz, tro = tsec[1], tsec[2], tsec[3]
tblob = data[tro:tro+tvsz]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
hits = []
for ins in md.disasm(tblob, BASE + tva):
    for op in ins.operands:
        if op.type == 2 and op.mem.base == 41:  # MEM with RIP (X86_REG_RIP=41)
            ea = ins.address + ins.size + op.mem.disp
            if str_va <= ea < str_va + 64:
                hits.append((ins.address, ins.mnemonic, ins.op_str, ea))
for h in hits[:20]: print(f"  {h[0]:x}: {h[1]} {h[2]} -> {h[3]:x}")
print(f"{len(hits)} refs")
