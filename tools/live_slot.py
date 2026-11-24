import ctypes, struct, subprocess, sys

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
if pid is None:
    print("jogo nao rodando"); sys.exit(1)
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
if not h:
    print("OpenProcess falhou"); sys.exit(1)

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

def u64(a):
    b = read(a, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

GA = 0x2238f108be8
TRUCK = 0x2238c8f4858
T = [0x2238c9eaf58, 0x2238c9ec228, 0x2238c9ed4f8]

slot = u64(GA + 0xC8)
print("game_actor+0xC8 (slot do ator) = 0x%X" % (slot or 0))
ga_truck = u64(GA + 0x18)
print("game_actor+0x18 (cavalo) = 0x%X (esperado 0x2238C8F4858)" % (ga_truck or 0))
towed = u64(TRUCK + 0x1580)
st = read(TRUCK + 0x14B8, 4)
print("truck+0x1580 (towed) = 0x%X  state+0x14B8 = %s" % (towed or 0, st.hex() if st else "?"))
for i, a in enumerate(T):
    j = u64(a + 0xFB0); p = u64(a + 0x1050); s = u64(a + 0x1058)
    r = u64(a + 0x1158); b1 = read(a + 0x1168, 1); b2 = read(a + 0x1228, 1)
    print("T%d %012X joint=%012X parent=%012X slave=%012X rec=%012X parked=%s reg=%s" % (
        i, a, j or 0, p or 0, s or 0, r or 0, b1.hex() if b1 else "?", b2.hex() if b2 else "?"))
