import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
TRO, TVA, TVSZ = 0x400, 0x1000, 0x1df0d70
tblob = data[TRO:TRO+TVSZ]
md = Cs(CS_ARCH_X86, CS_MODE_64)

# keep instructions near our targets, preserving global alignment
want = [(0x14002d7fc, "scale_x"), (0x14002d91c, "scale_y")]
buf = {}
for ins in md.disasm(tblob, BASE + TVA):
    for va, tag in want:
        if va - 0x90 <= ins.address <= va + 0x30:
            buf.setdefault(va, []).append(ins)

for va, tag in want:
    print(f"===== {tag} context =====")
    for ins in buf.get(va, []):
        mark = " <<<" if ins.address == va else ""
        print(f"{ins.address:x}: {ins.mnemonic:8} {ins.op_str}{mark}")
