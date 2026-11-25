import ctypes, struct, sys
from ctypes import wintypes

k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi
import subprocess
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = int([l for l in out.splitlines() if l.lower().startswith('"eurotrucks2')][0].split('","')[1])
h = k32.OpenProcess(0x1F0FFF, False, pid)
mods = (ctypes.c_void_p * 1024)(); need = wintypes.DWORD()
psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
exe_base = dll_base = dll_size = 0
class MI(ctypes.Structure):
    _fields_ = [("b", ctypes.c_void_p), ("s", wintypes.DWORD), ("e", ctypes.c_void_p)]
for i in range(need.value // 8):
    n = ctypes.create_string_buffer(260)
    psapi.GetModuleFileNameExA(h, ctypes.c_void_p(mods[i]), n, 260)
    p = n.value.decode().lower(); info = MI()
    psapi.GetModuleInformation(h, ctypes.c_void_p(mods[i]), ctypes.byref(info), 24)
    if p.endswith("eurotrucks2.exe"): exe_base = info.b
    if "br_controle_de_carretas" in p: dll_base, dll_size = info.b, info.s

def read(a, s):
    b = ctypes.create_string_buffer(s); g = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, s, ctypes.byref(g))
    return b.raw[:g.value] if ok else None
def u64(a):
    b = read(a, 8); return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None
def f32(a):
    b = read(a, 4); return struct.unpack("<f", b)[0] if b and len(b) == 4 else None
def heapish(p): return p and 0x10000 < p < 0x7FFFFFF0000 and not (exe_base <= p < exe_base + 0x4000000)

region = read(dll_base, dll_size)
containers = set()
for i in range(0, dll_size - 8, 8):
    v = struct.unpack_from("<Q", region, i)[0]
    if not heapish(v): continue
    arr = u64(v + 8); cnt = u64(v + 0x10)
    if not heapish(arr) or not cnt or cnt > 0x4000: continue
    d = read(arr, 8)
    if d and heapish(struct.unpack("<Q", d)[0]): containers.add((v, arr, cnt))

from collections import Counter
own_hist = Counter(); pos_hist = Counter(); own_pos = {}
rec_hist = Counter()
for c, arr, cnt in containers:
    d = read(arr, cnt * 8)
    if not d: continue
    for el in struct.unpack_from("<{}Q".format(cnt), d):
        if el == 0: continue
        own = u64(el + 0x88); rec = u64(el + 0x80)
        x = f32(el + 0x28); z = f32(el + 0x2C)
        own_hist[own] += 1; rec_hist[rec] += 1
        gx = int(x/10) if x is not None else 0; gz = int(z/10) if z is not None else 0
        pos_hist[(gx, gz)] += 1
        own_pos.setdefault(own, Counter())[(gx, gz)] += 1
print("containers:", len(containers))
print("top donos por n de elementos:")
for o, n in own_hist.most_common(8):
    vt = u64(o + 0) if o else 0
    tag = "exe+0x{:X}".format(vt - exe_base) if vt and exe_base <= vt < exe_base + 0x4000000 else str(vt)
    print("  0x{:X} vtbl={} x{}".format(o or 0, tag, n))
print("top records (el+0x80):")
for r, n in rec_hist.most_common(5):
    print("  0x{:X} x{}".format(r or 0, n))
print("top posicoes (grade 10u):")
for p, n in pos_hist.most_common(6):
    print("  {} x{}".format(p, n))
