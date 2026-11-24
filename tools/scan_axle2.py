import struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RBP, X86_REG_RSP, X86_REG_RIP

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
tblob = data[0x400:0x400+0x1df0d70]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

hits = {0x5C8: [], 0x5D0: []}
for ins in md.disasm(tblob, BASE + 0x1000):
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.disp in (0x5C8, 0x5D0) \
           and op.mem.base not in (X86_REG_RBP, X86_REG_RSP, X86_REG_RIP, 0):
            hits[op.mem.disp].append((ins.address, ins.mnemonic, ins.op_str, op.mem.base))
        if op.type == X86_OP_MEM: break  # first mem op only is enough

from capstone.x86_const import X86_REG_RBX, X86_REG_RCX, X86_REG_RDI, X86_REG_RAX, X86_REG_RDX, X86_REG_RSI, X86_REG_R8, X86_REG_R9, X86_REG_R14, X86_REG_R15
regname = {X86_REG_RAX:"rax",X86_REG_RBX:"rbx",X86_REG_RCX:"rcx",X86_REG_RDX:"rdx",
           X86_REG_RSI:"rsi",X86_REG_RDI:"rdi",X86_REG_R8:"r8",X86_REG_R9:"r9",
           X86_REG_R14:"r14",X86_REG_R15:"r15"}
for want in (0x5C8, 0x5D0):
    print(f"===== +{want:x} non-stack =====")
    for a,m,o,b in hits[want][:40]:
        print(f"  {a:x}: {m} {o}   [base={regname.get(b,b)}]")
    print(f"  total={len(hits[want])}")
