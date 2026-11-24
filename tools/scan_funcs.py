import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RBP, X86_REG_RSP, X86_REG_RIP

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()

# .pdata: va 0x143749000 raw 0x3094800, vsz 0x166bc0 -> RUNTIME_FUNCTION triples
PRAW, PVA = 0x3094800, 0x143749000
n = 0x166bc0 // 12
funcs = []
for i in range(n):
    b, e, u = struct.unpack_from("<III", data, PRAW + i * 12)
    funcs.append((BASE + b, BASE + e))

tblob_va = BASE + 0x1000
def code(va):
    off = va - BASE - 0xC00
    return data[off:]

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

want = {0x5C8: 0, 0x5D0: 0, 0x6C8: 0}
samples = {k: [] for k in want}
for b, e in funcs:
    for ins in md.disasm(code(b), b, count=(e - b)):
        if ins.address >= e: break
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.disp in want \
               and op.mem.base not in (X86_REG_RBP, X86_REG_RSP, X86_REG_RIP, 0):
                want[op.mem.disp] += 1
                if len(samples[op.mem.disp]) < 12:
                    samples[op.mem.disp].append((ins.address, ins.mnemonic, ins.op_str))
for k in want:
    print(f"+{k:x}: {want[k]} refs")
    for a, m, o in samples[k]:
        print(f"   {a:x}: {m} {o}")
