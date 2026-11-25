import ctypes, struct, sys
from ctypes import wintypes

k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

import subprocess
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = int([l for l in out.splitlines() if l.lower().startswith('"eurotrucks2')][0].split('","')[1])
print("PID", pid)
h = k32.OpenProcess(0x1F0FFF, False, pid)

mods = (ctypes.c_void_p * 1024)()
need = wintypes.DWORD()
psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
exe_base = dll_base = dll_size = 0
class MODINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", ctypes.c_void_p)]
for i in range(need.value // 8):
    name = ctypes.create_string_buffer(260)
    psapi.GetModuleFileNameExA(h, ctypes.c_void_p(mods[i]), name, 260)
    p = name.value.decode().lower()
    info = MODINFO()
    psapi.GetModuleInformation(h, ctypes.c_void_p(mods[i]), ctypes.byref(info), ctypes.sizeof(info))
    if p.endswith("eurotrucks2.exe"): exe_base = info.lpBaseOfDll
    if "br_controle_de_carretas" in p:
        dll_base = info.lpBaseOfDll; dll_size = info.SizeOfImage
print("exe 0x{:X}  dll 0x{:X}+0x{:X}".format(exe_base, dll_base, dll_size))

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

def u64(addr):
    b = read(addr, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

def heapish(p):
    return p is not None and 0x10000 < p < 0x7FFFFFF0000 and not (exe_base <= p < exe_base + 0x4000000)

def valid_container(c):
    arr = u64(c + 8); cnt = u64(c + 0x10)
    if not heapish(arr) or cnt is None or cnt == 0 or cnt > 0x4000: return None
    d = read(arr, 8)
    if d is None: return None
    e0 = struct.unpack("<Q", d)[0]
    if not heapish(e0): return None
    return (arr, cnt)

# varre .data/.bss da DLL: todo qword que aponta pra container valido
region = read(dll_base, dll_size)
containers = {}
for i in range(0, dll_size - 8, 8):
    v = struct.unpack_from("<Q", region, i)[0]
    if not heapish(v): continue
    r = valid_container(v)
    if r: containers[v] = r
print("containers validos encontrados:", len(containers))

total = dead = live = freed = 0
owns = set()
vt_count = {}
for c, (arr, cnt) in containers.items():
    d = read(arr, cnt * 8)
    if d is None or len(d) < cnt * 8: continue
    for el in struct.unpack_from("<{}Q".format(cnt), d):
        if el == 0: continue
        total += 1
        vt = u64(el + 0)
        own = u64(el + 0x88)
        key = "exe+0x{:X}".format(vt - exe_base) if vt and exe_base <= vt < exe_base + 0x4000000 else vt
        vt_count[key] = vt_count.get(key, 0) + 1
        if vt is None or not (exe_base <= vt < exe_base + 0x4000000):
            freed += 1
            continue
        if own is None or own == 0 or (dll_base <= own < dll_base + dll_size):
            dead += 1
        else:
            live += 1
            owns.add(own)
print("elementos: total={} mortos-carimbados={} vivos={} sem-vtbl={}".format(total, dead, live, freed))
print("donos vivos unicos:", len(owns))
print("vtables:", {k if isinstance(k,int) else k: v for k, v in sorted(vt_count.items(), key=lambda x:-x[1])[:8]})
