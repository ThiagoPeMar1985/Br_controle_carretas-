import ctypes, struct, subprocess

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = int([l for l in out.splitlines() if l.lower().startswith('"eurotrucks2')][0].split('","')[1])
h = k32.OpenProcess(0x0010 | 0x0400 | 0x0020, False, pid)

def read(a, s):
    b = ctypes.create_string_buffer(s); g = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, s, ctypes.byref(g))
    return b.raw[:g.value] if ok else None

# enum modules -> achar br_controle_de_carretas.dll
th = k32.CreateToolhelp32Snapshot(0x18, pid)
class ME(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_ulong),("m", ctypes.c_ulong*3),("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", ctypes.c_ulong),("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char*256),("szExePath", ctypes.c_char*260)]
me = ME(); me.dwSize = ctypes.sizeof(ME)
ok = k32.Module32First(th, ctypes.byref(me))
dll_base = dll_size = None
while ok:
    if b"br_controle" in me.szModule.lower():
        dll_base, dll_size = me.modBaseAddr, me.modBaseSize
        break
    ok = k32.Module32Next(th, ctypes.byref(me))
k32.CloseHandle(th)
print(f"dll={dll_base and hex(dll_base)} size={dll_size and hex(dll_size)}")

# ler .data/.bss da DLL inteira e procurar a tripla de ponteiros de carreta
trailers = [0x1fd3c92daf8, 0x1fd3c92edc8, 0x1fd3c930098]
blob = read(dll_base, dll_size)
if blob:
    pat = b"".join(struct.pack("<Q", t) for t in trailers)
    i = blob.find(pat)
    print(f"tripla consecutiva em dll+{i:#x}" if i >= 0 else "tripla consecutiva nao achada")
    # tambem procurar cada ponteiro individualmente
    for t in trailers:
        j = blob.find(struct.pack("<Q", t))
        print(f"  {t:#x} -> dll+{j:#x}" if j >= 0 else f"  {t:#x} nao achado")
