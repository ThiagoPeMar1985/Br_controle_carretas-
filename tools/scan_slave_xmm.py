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

md = Cs(CS_ARCH_X86, CS_MODE_64)
# stores XMM com disp32 cobrindo +0x1050..+0x1058: disp alvo 0x1048/0x1050 (16 bytes)
hits = []
for disp in (0x1048, 0x1050, 0x1058):
    pat = struct.pack("<i", disp)
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        # instrução termina no disp+4; prefixos xmm store: 0F 29 / 0F 11 (com prefixos 66/F2/F3/REX)
        for back in range(3, 8):
            j = i + 4 - back
            if j < 1:
                continue
            b = data[j:i + 4]
            if b[-2:-1] in (b"\x29", b"\x11") and (b[-3] == 0x0F or b[-4:-2] in (b"\x66\x0f", b"\xf2\x0f", b"\xf3\x0f")):
                va = off2va(j)
                if va:
                    off = None
                    for name, vaddr, vsize, raddr, rsize in secs:
                        if vaddr <= (va - BASE) < vaddr + vsize:
                            off = raddr + ((va - BASE) - vaddr)
                    insn = next(md.disasm(data[off:off + 16], va), None)
                    if insn and ("mov" in insn.mnemonic) and ("[" in insn.op_str.split(",")[-1]):
                        hits.append((va, insn.mnemonic, insn.op_str))
                break
        start = i + 1
hits.sort()
print("stores XMM cobrindo o bloco slave:", len(hits))
for va, m, o in hits[:40]:
    print("  0x{:X}: {:7s} {}".format(va, m, o))
