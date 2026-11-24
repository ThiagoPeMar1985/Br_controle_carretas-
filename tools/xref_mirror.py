import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000
data = open(EXE, "rb").read()

pe = struct.unpack_from("<I", data, 0x3C)[0]
sec_off = pe + 0x18 + struct.unpack_from("<H", data, pe + 0x14)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
secs = []
for i in range(nsec):
    o = sec_off + i * 40
    name = data[o:o+8].rstrip(b"\0").decode("ascii","replace")
    vsize, va, rsize, roff = struct.unpack_from("<IIII", data, o+8)
    secs.append((name, va, vsize, roff, rsize))

def sec_of_file(off):
    for n,va,vsz,ro,rs in secs:
        if ro <= off < ro+rs: return n, va, vsz, ro
    return None

targets = [0x14228F058, 0x14228F098]

# 1) data refs: qword == target anywhere in .rdata/.data
for tgt in targets:
    hits = []
    for n,va,vsz,ro,rs in secs:
        if n not in (".rdata", ".data", "_RDATA"): continue
        blob = data[ro:ro+rs]
        pat = struct.pack("<Q", tgt)
        i = 0
        while True:
            i = blob.find(pat, i)
            if i < 0: break
            hits.append(BASE + va + i)
            i += 1
    print(f"data refs to {tgt:x}: {[hex(h) for h in hits]}")

# 2) code refs: rip-relative lea/mov in .text
tsec = next(s for s in secs if s[0]==".text")
tva, tvsz, tro = tsec[1], tsec[2], tsec[3]
tblob = data[tro:tro+tvsz]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for tgt in targets:
    print(f"\ncode refs to {tgt:x}:")
    cnt = 0
    for ins in md.disasm(tblob, BASE + tva):
        if ins.mnemonic in ("lea","mov","cmp") and "[rip" in ins.op_str:
            # parse disp
            import re
            m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
            if m:
                disp = int(m.group(2),16) * (1 if m.group(1)=="+" else -1)
                ea = ins.address + ins.size + disp
                if ea == tgt:
                    print(f"  {ins.address:x}: {ins.mnemonic} {ins.op_str}")
                    cnt += 1
                    if cnt > 12: break
    print(f"  ({cnt} shown)")
