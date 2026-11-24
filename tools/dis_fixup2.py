from capstone import Cs, CS_ARCH_X86, CS_MODE_64
EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
md = Cs(CS_ARCH_X86, CS_MODE_64)
start = 0x1405DB4A0
off = start - BASE - 0xC00
for ins in md.disasm(data[off:off+0x500], start):
    print(f"{ins.address:x}: {ins.mnemonic:8} {ins.op_str}")
