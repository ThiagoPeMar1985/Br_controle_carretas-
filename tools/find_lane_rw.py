import struct, bisect
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

pdata = next(s for s in secs if s[0] == ".pdata")
text  = next(s for s in secs if s[0] == ".text")
po, pv, psz = pdata[3], pdata[1], pdata[2]
funcs = []
for i in range(0, psz, 12):
    b, e, _ = struct.unpack_from("<III", data, po + i)
    funcs.append((BASE + b, BASE + e))
funcs.sort()
starts = [f[0] for f in funcs]

def func_of(va):
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0 and funcs[i][0] <= va < funcs[i][1]:
        return funcs[i]
    return None

to, tv, tsz = text[3], text[1], text[2]
code = data[to:to+tsz]
md = Cs(CS_ARCH_X86, CS_MODE_64)

# percorre funcao por funcao (decodifica do inicio -> alinhado)
readers88_90 = {}   # fn -> list of insns lendo +0x88 e +0x90
writers28 = {}      # fn -> insns escrevendo [reg+0x28]
for fb, fe in funcs:
    rva_b = fb - BASE
    off = rva_b - tv + 0
    if off < 0 or off >= len(code): continue
    chunk = code[off : off + min(fe - fb, 0x4000)]
    r88 = r90 = w28 = w20 = False
    for insn in md.disasm(chunk, fb):
        ops = insn.op_str
        if "[rip" in ops: continue
        if "+ 0x88]" in ops and insn.mnemonic in ("mov",) and not ops.startswith("qword ptr ["):
            r88 = True
        if "+ 0x90]" in ops and insn.mnemonic.startswith("movss"):
            r90 = True
        if insn.mnemonic == "mov" and ops.startswith("qword ptr [") and "+ 0x28]," in ops:
            w28 = True
        if insn.mnemonic == "mov" and ops.startswith("qword ptr [") and "+ 0x20]," in ops:
            w20 = True
    if r88 and r90:
        readers88_90[fb] = (w28, w20)

for f in sorted(readers88_90):
    w = readers88_90[f]
    print("fn 0x{:X}  le+0x88/+0x90  escreve+0x28={} +0x20={}".format(f, w[0], w[1]))
print("total funcoes leitoras de elemento:", len(readers88_90))
