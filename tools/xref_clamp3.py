import struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()

# show full message context around the string
idx = data.find(b"out of the allowed range")
print(repr(data[idx-60:idx+80]))

# qword data refs to the string VA (0x142291122)
tgt = 0x142291122
pat = struct.pack("<Q", tgt)
i = 0
while True:
    i = data.find(pat, i)
    if i < 0: break
    print(f"data ref @ file {i:x}")
    i += 1

# also try the message START (find where the full format string begins)
start = idx
while data[start-1] != 0: start -= 1
end = idx
while data[end] != 0: end += 1
print("full str:", data[start:end])
# compute VA: .rdata raw 0x1df1200 -> va 0x141df2000
strva = 0x141DF2000 + (start - 0x1DF1200)
print(f"str VA = {strva:x}")

# code refs to strva
TRO, TVA, TVSZ = 0x400, 0x1000, 0x1df0d70
tblob = data[TRO:TRO+TVSZ]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for ins in md.disasm(tblob, BASE + TVA):
    m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
    if m:
        disp = int(m.group(2),16) * (1 if m.group(1)=="+" else -1)
        ea = ins.address + ins.size + disp
        if ea == strva:
            print(f"{ins.address:x}: {ins.mnemonic} {ins.op_str}")
# data refs to strva
pat = struct.pack("<Q", strva)
i = 0
while True:
    i = data.find(pat, i)
    if i < 0: break
    print(f"data ref @ file {i:x}")
    i += 1
