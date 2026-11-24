import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()

def off_of(va): return va - BASE + 0x400  # .text starts BASE+0x1000 at raw 0x400
start = 0x14002D780
end   = 0x14002D9A0
blob = data[off_of(start):off_of(end)]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for ins in md.disasm(blob, start):
    print(f"{ins.address:x}: {ins.mnemonic:8} {ins.op_str}")
