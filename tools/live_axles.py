import ctypes, struct, subprocess, sys

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
if pid is None:
    print("jogo nao esta rodando"); sys.exit(1)

h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
if not h:
    print("OpenProcess falhou"); sys.exit(1)

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

# exe base via primeiro modulo
th32 = ctypes.windll.kernel32.CreateToolhelp32Snapshot(0x18, pid)
class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_ulong), ("th32ModuleID", ctypes.c_ulong),
                ("th32ProcessID", ctypes.c_ulong), ("GlblcntUsage", ctypes.c_ulong),
                ("ProccntUsage", ctypes.c_ulong), ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize", ctypes.c_ulong), ("hModule", ctypes.c_void_p),
                ("szModule", ctypes.c_char * 256), ("szExePath", ctypes.c_char * 260)]
me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
base = 0
k32.Module32First(th32, ctypes.byref(me))
if b"eurotrucks2" in me.szModule.lower():
    base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
k32.CloseHandle(th32)
if not base:
    print("base nao achada"); sys.exit(1)
print(f"exe base = {base:#x}  PID={pid}")

# resolve base_ctrl pattern: 48 8b 3d ?? ?? ?? ?? 48 8b bf ?? ?? ?? ?? 48 8b 5f 18 48 8b 9b
pat = bytes.fromhex("48 8b 3d")
blob = read(base + 0x1000, 0x1df0d70)
addr = None
i = 0
while True:
    i = blob.find(pat, i)
    if i < 0: break
    if blob[i+7:i+10] == bytes.fromhex("48 8b bf") and blob[i+14:i+20] == bytes.fromhex("48 8b 5f 18 48 8b"):
        addr = base + 0x1000 + i; break
    i += 1
if addr is None:
    print("padrao base_ctrl nao achado"); sys.exit(1)
bc_ptr_addr = addr + struct.unpack_from("<i", blob, i + 3)[0] + 7
ga_off = struct.unpack_from("<i", blob, i + 10)[0]
base_ctrl = u64(bc_ptr_addr)
ga = u64(base_ctrl + ga_off) if base_ctrl else None
print(f"base_ctrl={base_ctrl and hex(base_ctrl)} game_actor={ga and hex(ga)}")

truck = u64(ga + 0x18) if ga else None
t = u64(ga + 0xC8) if ga else None
print(f"truck={truck and hex(truck)}  T0={t and hex(t)}")

def dump_obj(tag, addr, lo, hi, step=16):
    blob = read(addr + lo, hi - lo)
    if not blob: print(f"  {tag}: ilegivel"); return
    for j in range(0, hi - lo, step):
        row = blob[j:j+step]
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print(f"  {tag}+{lo+j:04x}: {row.hex(' ')}  {asc}")

n = 0
while t and n < 4:
    print(f"\n===== trailer{n} {t:#x} =====")
    cd  = u64(t + 0x200)
    ctl = u64(t + 0x6C8)
    print(f"  +0x200 chassis={cd and hex(cd)}  +0x6C8 ctl={ctl and hex(ctl)}  +0x6C0 steer={u64(t+0x6C0) and hex(u64(t+0x6C0))}  +0x4D4 susp={f32(t+0x4D4)}")
    if ctl:
        print(f"  ctl: +0x8 count={u32(ctl+0x8)}  +0x18 prog={u64(ctl+0x18) and hex(u64(ctl+0x18))}  +0x20 pcount={u64(ctl+0x20)}  +0x38 mask={u32(ctl+0x38):#x}  +0x3C state={u8(ctl+0x3C)}")
    if cd:
        print("  -- chassis_data 0x500..0x700 --")
        dump_obj("cd", cd, 0x500, 0x700)
    t = u64(t + 0x1058)
    n += 1
