import struct

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
BASE = 0x140000000

data = open(EXE, "rb").read()

# PE sections
pe = struct.unpack_from("<I", data, 0x3C)[0]
sec_off = pe + 0x18 + struct.unpack_from("<H", data, pe + 0x14)[0]
secs = []
nsec = struct.unpack_from("<H", data, pe + 6)[0]
for i in range(nsec):
    o = sec_off + i * 40
    name = data[o:o+8].rstrip(b"\0").decode("ascii", "replace")
    vsize, va, rsize, roff = struct.unpack_from("<IIII", data, o + 8)
    secs.append((name, va, vsize, roff, rsize))
    print(f"{name:10} va={BASE+va:012x} vsz={vsize:x} raw={roff:x}")

def va2off(va):
    rva = va - BASE
    for name, sva, vsz, roff, rsz in secs:
        if sva <= rva < sva + vsz:
            return roff + (rva - sva)
    return None

for pat in [b"r_mirror_scale_x\0", b"r_mirror_scale_y\0", b"r_mirror_group\0"]:
    idx = data.find(pat)
    print(f"\n{pat[:-1]}: file_off={idx:x} va={BASE + next(s[1] for s in secs if s[0]=='.rdata') + (idx - next(s[3] for s in secs if s[0]=='.rdata')):x}" if idx >= 0 else f"{pat}: NOT FOUND")
    if idx >= 0:
        # compute VA via section containing idx
        for name, sva, vsz, roff, rsz in secs:
            if roff <= idx < roff + rsz:
                print(f"  VA = {BASE + sva + (idx - roff):x}")
