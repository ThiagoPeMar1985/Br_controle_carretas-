import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
TRO, TVA, TVSZ = 0x400, 0x1000, 0x1df0d70
tblob = data[TRO:TRO+TVSZ]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# name buffer dest was 0x142d859a8; print the whole init block: 0x14002d7f0 .. ~0x14002d8f0
keep = []
for ins in md.disasm(tblob, BASE + TVA):
    if 0x14002d7f0 <= ins.address <= 0x14002d8f0:
        # resolve rip targets
        tgt = ""
        for op in ins.operands:
            if op.type == 2 and op.mem.base == 41:
                ea = ins.address + ins.size + op.mem.disp
                tgt = f"  -> {ea:x}"
                b = struct.unpack_from("<f", data, ea - BASE + (TRO - TVA) + (0x1df1200 - 0x1df2000) if False else 0)[0] if False else None
        keep.append((ins.address, ins.mnemonic, ins.op_str, tgt))
for a, m, o, t in keep:
    print(f"{a:x}: {m:8} {o}{t}")
