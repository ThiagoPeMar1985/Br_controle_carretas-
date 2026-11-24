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
    name = data[off:off + 8].rstrip(b"\0").decode()
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    secs.append((name, vaddr, vsize, raddr, rsize))

def off2va(i):
    for name, vaddr, vsize, raddr, rsize in secs:
        if raddr <= i < raddr + rsize:
            return BASE + vaddr + (i - raddr)
    return None

# stores com disp32=0x1058: 48 89 (mov qword [base+disp], reg) e 48 8B (load - so p/ referencia)
# mod=10 -> modrm & 0xC0 == 0x80; rm != 4/5 (sem SIB)
md = Cs(CS_ARCH_X86, CS_MODE_64)
hits = []
start = 0
pat_prefixes = [b"\x48\x89", b"\x4c\x89", b"\x49\x89"]
while True:
    idx = data.find(b"\x58\x10\x00\x00", start)
    if idx < 0:
        break
    # tenta as 3 formas de prefixo terminando 4 bytes antes do disp
    for plen in (3,):
        i = idx - plen
        if i < 0:
            continue
        pre = data[i:i + 2]
        modrm = data[i + 2]
        if pre not in (b"\x48\x89", b"\x4c\x89", b"\x49\x89", b"\x4c\x8b", b"\x48\x8b"):
            continue
        if modrm & 0xC0 != 0x80:
            continue
        rm = modrm & 0x07
        if rm in (4, 5):
            continue
        va = off2va(i)
        if va:
            hits.append((va, pre.hex(), modrm))
    start = idx + 1

hits.sort()
print("acessos a [reg+0x1058] (slave):", len(hits))
for va, pre, modrm in hits:
    # disassembla a instrucao pra classificar leitura/escrita
    code = data[(va - BASE) - (va - BASE) % 1:]
    off = None
    for name, vaddr, vsize, raddr, rsize in secs:
        if vaddr <= (va - BASE) < vaddr + vsize:
            off = raddr + ((va - BASE) - vaddr)
    insn = next(md.disasm(data[off:off + 16], va), None)
    if insn:
        print("  0x{:X}: {:7s} {}".format(va, insn.mnemonic, insn.op_str))
