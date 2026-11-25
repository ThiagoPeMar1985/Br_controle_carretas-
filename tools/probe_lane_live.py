import ctypes, struct, sys
from ctypes import wintypes

k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi
PROCESS_ALL = 0x1F0FFF

import subprocess
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
if pid is None:
    print("jogo fechado"); sys.exit(1)
print("PID", pid)

h = k32.OpenProcess(PROCESS_ALL, False, pid)
if not h:
    print("OpenProcess falhou", ctypes.get_last_error()); sys.exit(1)

# enumera modulos -> base do exe e da dll
mods = (ctypes.c_void_p * 1024)()
need = wintypes.DWORD()
psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
exe_base = dll_base = dll_size = 0
mi = wintypes.CHAR * 260
class MODINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", ctypes.c_void_p)]
for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
    name = ctypes.create_string_buffer(260)
    psapi.GetModuleFileNameExA(h, ctypes.c_void_p(mods[i]), name, 260)
    p = name.value.decode().lower()
    info = MODINFO()
    psapi.GetModuleInformation(h, ctypes.c_void_p(mods[i]), ctypes.byref(info), ctypes.sizeof(info))
    if p.endswith("eurotrucks2.exe"):
        exe_base = info.lpBaseOfDll
        print("exe base 0x{:X}".format(exe_base))
    if "br_controle_de_carretas" in p:
        dll_base = info.lpBaseOfDll; dll_size = info.SizeOfImage
        print("dll base 0x{:X} size 0x{:X}".format(dll_base, dll_size))
if not dll_base:
    print("dll nao encontrada"); sys.exit(1)

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

def u64(addr):
    b = read(addr, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

def is_heapish(p):
    return p is not None and 0x10000 < p < 0x7FFFFFF0000 and not (exe_base <= p < exe_base + 0x4000000)

# varre a area de dados da DLL atras de lane_containers[128]:
# 128 qwords consecutivas; as nao-nulas apontam pra {+8 arr, +0x10 count<=0x10000}
region = read(dll_base, dll_size)
found_c = None
for i in range(0, dll_size - 128 * 8, 8):
    q = struct.unpack_from("<128Q", region, i)
    nz = [v for v in q if v != 0]
    if len(nz) < 2 or len(nz) > 64:
        continue
    if not all(is_heapish(v) for v in nz):
        continue
    # valida o primeiro como container {+8 arr, +0x10 count}
    arr = u64(nz[0] + 8); cnt = u64(nz[0] + 0x10)
    if arr and cnt is not None and 0 < cnt <= 0x10000:
        found_c = dll_base + i
        print("lane_containers @ dll+0x{:X}  ({} ptrs nao-nulos)".format(i, len(nz)))
        break
if found_c is None:
    print("lane_containers nao achado - tentando varredura solta")
    sys.exit(1)

cq = struct.unpack_from("<128Q", read(found_c, 128 * 8))
total = dead_stamp = live = freed_el = 0
el_vtbls = {}
own_set = set()
for c in cq:
    if c == 0: continue
    arr = u64(c + 8); cnt = u64(c + 0x10)
    if arr is None or cnt is None or cnt == 0 or cnt > 0x10000: continue
    data = read(arr, cnt * 8)
    if data is None or len(data) < cnt * 8: continue
    els = struct.unpack_from("<{}Q".format(cnt), data)
    for el in els:
        if el == 0: continue
        total += 1
        own = u64(el + 0x88)
        vt = u64(el + 0)
        el_vtbls[vt] = el_vtbls.get(vt, 0) + 1
        if vt is None or not (exe_base <= vt < exe_base + 0x4000000):
            freed_el += 1
        if own == 0:
            dead_stamp += 1
        elif own is not None and dll_base <= own < dll_base + dll_size:
            dead_stamp += 1   # dono-falso = dentro da nossa DLL
        else:
            live += 1
            if own: own_set.add(own)
print("\n=== elementos nas containers rastreadas ===")
print("total={} carimbados-mortos={} vivos={} elem-sem-vtbl={}".format(total, dead_stamp, live, freed_el))
print("donos unicos vivos:", len(own_set))
print("vtables de elemento vistas:")
for vt, n in sorted(el_vtbls.items(), key=lambda x: -x[1])[:6]:
    tag = "exe+0x{:X}".format(vt - exe_base) if vt and exe_base <= vt < exe_base + 0x4000000 else "fora-do-exe {:X}".format(vt or 0)
    print("  {:24s} x{}".format(tag, n))
