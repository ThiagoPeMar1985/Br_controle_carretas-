import struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
TRO, TVA, TVSZ = 0x400, 0x1000, 0x1df0d70
tblob = data[TRO:TRO+TVSZ]
md = Cs(CS_ARCH_X86, CS_MODE_64)

tgt = 0x142291122  # "out of the allowed range"
for ins in md.disasm(tblob, BASE + TVA):
    m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
    if m:
        disp = int(m.group(2),16) * (1 if m.group(1)=="+" else -1)
        ea = ins.address + ins.size + disp
        if tgt <= ea < tgt + 32:
            print(f"{ins.address:x}: {ins.mnemonic} {ins.op_str}  -> {ea:x}")
