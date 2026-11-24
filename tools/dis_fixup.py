import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
tblob = data[0x400:0x400+0x1df0d70]
md = Cs(CS_ARCH_X86, CS_MODE_64)

start = 0x1405DB490
# disasm a window
blob = data[start - BASE - 0x1000 + 0x400 : start - BASE - 0x1000 + 0x400 + 0x600]
for ins in md.disasm(blob, start):
    print(f"{ins.address:x}: {ins.mnemonic:8} {ins.op_str}")
    if ins.address > start + 0x550: break
