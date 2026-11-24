import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
tblob = data[0x400:0x400+0x1df0d70]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

for want in (0x5C8, 0x5D0, 0x6C8, 0x200):
    print(f"===== disp {want:#x} =====")
    n = 0
    for ins in md.disasm(tblob, BASE + 0x1000):
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.disp == want and op.mem.base != 41:
                print(f"  {ins.address:x}: {ins.mnemonic} {ins.op_str}")
                n += 1
                break
        if n >= 25: break
    print(f"  ({n} shown)")
