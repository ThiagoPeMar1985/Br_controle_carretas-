import ctypes, struct, subprocess, sys

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None
def u64(a):
    b = read(a, 8); return struct.unpack("<Q", b)[0] if b else None
def u32(a):
    b = read(a, 4); return struct.unpack("<I", b)[0] if b else None
def u8(a):
    b = read(a, 1); return b[0] if b else None
def f32(a):
    b = read(a, 4); return struct.unpack("<f", b)[0] if b else None

th32 = k32.CreateToolhelp32Snapshot(0x18, pid)
class ME(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_ulong), ("th32ModuleID", ctypes.c_ulong),
                ("th32ProcessID", ctypes.c_ulong), ("g", ctypes.c_ulong), ("p", ctypes.c_ulong),
                ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", ctypes.c_ulong),
                ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_char*256), ("szExePath", ctypes.c_char*260)]
me = ME(); me.dwSize = ctypes.sizeof(ME)
k32.Module32First(th32, ctypes.byref(me))
base = me.modBaseAddr
k32.CloseHandle(th32)
print(f"exe base = {base:#x}")

blob = bytearray()
step = 0x800000
for off in range(0, 0x1df0d70, step):
    part = read(base + 0x1000 + off, min(step, 0x1df0d70 - off))
    blob += part if part else bytes(min(step, 0x1df0d70 - off))
blob = bytes(blob)
pat = bytes.fromhex("48 8b 3d")
hits = []
i = 0
while True:
    i = blob.find(pat, i)
    if i < 0: break
    if blob[i+7:i+10] == bytes.fromhex("48 8b bf") and blob[i+14:i+20] == bytes.fromhex("48 8b 5f 18 48 8b"):
        hits.append(i)
    i += 1
print(f"{len(hits)} pattern sites")
for i in hits:
    addr = base + 0x1000 + i
    bc_ptr_addr = addr + struct.unpack_from("<i", blob, i + 3)[0] + 7
    ga_off = struct.unpack_from("<i", blob, i + 10)[0]
    bc = u64(bc_ptr_addr)
    ga = u64(bc + ga_off) if bc else None
    t0 = u64(ga + 0xC8) if ga else None
    print(f"  site {addr:#x}: bc_ptr@{bc_ptr_addr:#x}={bc and hex(bc)} ga_off={ga_off:#x} ga={ga and hex(ga)} T0={t0 and hex(t0)}")
