import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
off = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
secs = {}
for _ in range(nsec):
    name = data[off:off+8].rstrip(b"\0").decode("ascii","replace")
    vsz, va, rsz, roff = struct.unpack_from("<IIII", data, off + 8)
    secs[name] = (va, roff, rsz, vsz)
    off += 40
IMG = 0x140000000

def raw_of(va):
    for name,(sv,ro,rs,_) in secs.items():
        if sv <= va - IMG < sv + rs:
            return ro + (va - IMG - sv)
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False

for lo, hi in [(0x7771e0, 0x7773c0), (0x85cc00, 0x85cd80)]:
    print(f"\n===== {lo:#x}..{hi:#x} =====")
    r = raw_of(IMG + lo)
    blob = data[r:r + (hi - lo)]
    for ins in md.disasm(blob, IMG + lo):
        mark = " <==" if "4d4" in ins.op_str.lower() else ""
        print(f"  {ins.address:#x}: {ins.mnemonic} {ins.op_str}{mark}")
        # resolver rip-relative
        if "rip" in ins.op_str:
            try:
                disp = int(ins.op_str.split("0x")[-1].rstrip("]"), 16)
                tgt = ins.address + ins.size + disp
                fr = raw_of(tgt)
                if fr is not None:
                    f = struct.unpack_from("<f", data, fr)[0]
                    print(f"        -> {tgt:#x} = float {f}")
            except Exception: pass
