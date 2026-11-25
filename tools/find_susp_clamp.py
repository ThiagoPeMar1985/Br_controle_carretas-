import struct

EXE = r"D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe"
data = open(EXE, "rb").read()

# PE headers -> .text e .rdata
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
sections = []
off = opt + struct.unpack_from("<H", data, pe + 20)[0]
for i in range(nsec):
    name = data[off:off+8].rstrip(b"\0").decode("ascii", "replace")
    vsz, va, rsz, roff = struct.unpack_from("<IIII", data, off + 8)
    sections.append((name, va, vsz, roff, rsz))
    off += 40
sec = {s[0]: s for s in sections}
text = sec[".text"]
base_img = 0x140000000

tva, traw, trsz = text[1], text[3], text[4]
tblob = data[traw:traw+trsz]

# disp32 = 4d4 -> bytes d4 04 00 00
hits = []
i = tblob.find(b"\xd4\x04\x00\x00")
while i >= 0:
    hits.append(traw + i)
    i = tblob.find(b"\xd4\x04\x00\x00", i + 1)
print(f"{len(hits)} refs brutas a +0x4D4")
for h in hits[:60]:
    va = base_img + text[1] + (h - traw)
    print(f"  exe+{va - base_img:#x}  ctx={data[h-6:h+8].hex(' ')}")
