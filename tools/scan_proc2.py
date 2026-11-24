import ctypes, struct, subprocess

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = int([l for l in out.splitlines() if l.lower().startswith('"eurotrucks2')][0].split('","')[1])
h = k32.OpenProcess(0x0010 | 0x0400 | 0x0020 | 0x0008, False, pid)

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p),("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", ctypes.c_ulong),("RegionSize", ctypes.c_size_t),
                ("State", ctypes.c_ulong),("Protect", ctypes.c_ulong),("Type", ctypes.c_ulong)]

def read(a, s):
    b = ctypes.create_string_buffer(s); g = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, s, ctypes.byref(g))
    return b.raw[:g.value] if ok else None

T0, T1, T2 = 0x1fd3c92daf8, 0x1fd3c92edc8, 0x1fd3c930098
pat1 = struct.pack("<Q", T0)

addr = 0x10000
mbi = MBI()
scanned = 0
while addr < 0x7FFFFFFFFFFF:
    n = k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
    if not n or not mbi.BaseAddress: break
    base, size = mbi.BaseAddress, mbi.RegionSize
    if mbi.State == 0x1000 and mbi.Protect in (4, 8, 0x40) and size <= 0x4000000:
        off = 0
        while off < size:
            chunk = read(base + off, min(0x400000, size - off))
            if chunk:
                j = chunk.find(pat1)
                while j >= 0:
                    a = base + off + j
                    ctx = read(a - 8, 0x48)
                    print(f"@{a:#x} type={mbi.Type:#x}: ...{ctx.hex(' ') if ctx else '?'}")
                    j = chunk.find(pat1, j + 1)
            off += 0x400000
        scanned += size
    nxt = base + size
    if nxt <= addr: break
    addr = nxt
print(f"scanned {scanned >> 20} MB")
