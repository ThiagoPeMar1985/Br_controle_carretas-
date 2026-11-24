import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()

def off_of(va):
    # .data: va 0x14273b000 raw 0x2739a00
    return 0x2739a00 + (va - 0x14273B000)

# dump descriptor 0x142D859A8 .. 0x142D85AE0
va = 0x142D859A8
o = off_of(va)
blob = data[o:o+0x140]
for i in range(0, 0x140, 16):
    row = blob[i:i+16]
    print(f"{va+i:x}: {row.hex(' ')}  {''.join(chr(b) if 32<=b<127 else '.' for b in row)}")

# the function ptr stored in desc: 0x140425440
print("\n=== func 0x140425440 ===")
TRO, TVA, TVSZ = 0x400, 0x1000, 0x1df0d70
tblob = data[TRO:TRO+TVSZ]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for ins in md.disasm(tblob, BASE + TVA):
    if 0x140425440 <= ins.address <= 0x140425500:
        print(f"{ins.address:x}: {ins.mnemonic:8} {ins.op_str}")
