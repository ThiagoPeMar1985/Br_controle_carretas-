import ctypes, struct, subprocess

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
if not pid: print("jogo fechado"); raise SystemExit
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)

def read(a, s):
    b = ctypes.create_string_buffer(s); g = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, s, ctypes.byref(g))
    return b.raw[:g.value] if ok else None
def u64(a):
    b = read(a, 8); return struct.unpack("<Q", b)[0] if b else None

class ME(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_ulong),("m", ctypes.c_ulong*3),("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", ctypes.c_ulong),("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char*256),("szExePath", ctypes.c_char*260)]
me = ME(); me.dwSize = ctypes.sizeof(ME)
th = k32.CreateToolhelp32Snapshot(0x18, pid)
k32.Module32First(th, ctypes.byref(me)); base = me.modBaseAddr; k32.CloseHandle(th)
print(f"base={base:#x} pid={pid}")

# read .text in 1MB chunks, count how many bytes actually came back
blob = bytearray()
fails = 0
for off in range(0, 0x1df0d70, 0x100000):
    part = read(base + 0x1000 + off, min(0x100000, 0x1df0d70 - off))
    if part is None: fails += 1; part = bytes(min(0x100000, 0x1df0d70 - off))
    blob += part
blob = bytes(blob)
print(f"read fails: {fails}, nonzero={sum(1 for x in blob[:0x100000] if x)}")

# scan for the tail fragment which is unique enough
for frag in [bytes.fromhex("48 8b 5f 18 48 8b 9b"), bytes.fromhex("48 8b bf")]:
    hits, i = [], 0
    while True:
        i = blob.find(frag, i)
        if i < 0: break
        hits.append(i); i += 1
        if len(hits) > 10: break
    print(f"frag {frag.hex()}: {len(hits)} hits -> {[hex(base+0x1000+x) for x in hits[:10]]}")
