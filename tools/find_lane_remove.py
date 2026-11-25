import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
secs = []
for i in range(nsec):
    off = opt + 0xF0 + i * 0x28
    name = data[off:off+8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))

def rva2off(rva):
    for name, vaddr, vsize, raddr, rsize in secs:
        if vaddr <= rva < vaddr + vsize:
            return raddr + (rva - vaddr)
    return None

# .pdata -> function ranges; .text -> code
pdata = next(s for s in secs if s[0] == ".pdata")
text  = next(s for s in secs if s[0] == ".text")
po, pv, psz = pdata[3], pdata[1], pdata[2]
funcs = []
for i in range(0, psz, 12):
    b, e, _ = struct.unpack_from("<III", data, po + i)
    funcs.append((BASE + b, BASE + e))
funcs.sort()

def func_of(va):
    import bisect
    i = bisect.bisect_right([f[0] for f in funcs], va) - 1
    if i >= 0 and funcs[i][0] <= va < funcs[i][1]:
        return funcs[i]
    return None

to, tv, tsz = text[3], text[1], text[2]
code = data[to:to+tsz]
md = Cs(CS_ARCH_X86, CS_MODE_64)

# escaneia o subsistema de lane: 0x1412B7000-0x1412BA000 + arredores
lo, hi = 0x1412B6000 - BASE, 0x1412BB000 - BASE
window = code[lo - tv : hi - tv]
hits = []
for insn in md.disasm(window, BASE + lo):
    # escritas em [reg + 0x28] (count do vetor do no)
    if insn.mnemonic == "mov" and "+ 0x28]" in insn.op_str and insn.op_str.split(",")[0].strip().endswith("0x28]"):
        f = func_of(insn.address)
        hits.append((insn.address, insn.mnemonic, insn.op_str, f[0] if f else 0))

for a, m, o, f in hits:
    print("0x{:X}: {} {:40s}  fn=0x{:X}".format(a, m, o, f))
print("total:", len(hits))
