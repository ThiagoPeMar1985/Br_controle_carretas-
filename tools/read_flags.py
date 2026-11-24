import ctypes, struct, subprocess

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = int([l for l in out.splitlines() if l.lower().startswith('"eurotrucks2')][0].split('","')[1])
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)

def read(a, s):
    b = ctypes.create_string_buffer(s); g = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, s, ctypes.byref(g))
    return b.raw[:g.value] if ok else None
def u64(a):
    b = read(a, 8); return struct.unpack("<Q", b)[0] if b else None

for i, fl in enumerate([0x1fa5827c740, 0x1fa5827c8c0, 0x1fa5827c980]):
    blob = read(fl, 8)
    print(f"flags trailer{i} @{fl:#x}: {blob.hex(' ') if blob else 'ilegivel'}")
