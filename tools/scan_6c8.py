import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RBP, X86_REG_RSP, X86_REG_RIP

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
tblob = data[0x400:0x400+0x1df0d70]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

for ins in md.disasm(tblob, BASE + 0x1000):
    memops = [op for op in ins.operands if op.type == X86_OP_MEM]
    for op in memops:
        if op.mem.disp == 0x6C8 and op.mem.base not in (X86_REG_RBP, X86_REG_RSP, X86_REG_RIP, 0):
            print(f"{ins.address:x}: {ins.mnemonic} {ins.op_str}")
