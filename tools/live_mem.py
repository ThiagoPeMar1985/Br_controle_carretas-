import ctypes, struct, sys

k32 = ctypes.windll.kernel32
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

# acha o PID do eurotrucks2
import subprocess
out = subprocess.check_output("tasklist /FI \"IMAGENAME eq eurotrucks2.exe\" /FO CSV", shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1])
        break
if pid is None:
    print("jogo nao encontrado"); sys.exit(1)
print("PID", pid)

h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
if not h:
    print("OpenProcess falhou", ctypes.get_last_error()); sys.exit(1)

objs = {
    "game_actor": 0x2798E57FAF8,
    "truck":      0x2798DAA60E8,
    "trailer0":   0x2798DAA8DE8,
    "trailer1":   0x2798DAAA0B8,
    "trailer2":   0x2798DAAB388,
}

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

def u64(addr):
    b = read(addr, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

tag = sys.argv[1] if len(sys.argv) > 1 else "snap"
out = open(r"C:\Users\PC\Documents\Euro Truck Simulator 2\br_mem_%s.txt" % tag, "w")

# grafo vivo + campos-chave
for name, addr in objs.items():
    parent = u64(addr + 0x1050)
    slave  = u64(addr + 0x1058)
    rec    = u64(addr + 0x1158)
    b1168  = read(addr + 0x1168, 1)
    b1228  = read(addr + 0x1228, 1)
    out.write("%s %012X parent=%012X slave=%012X rec=%012X +1168=%s +1228=%s\n" % (
        name, addr, parent or 0, slave or 0, rec or 0,
        b1168.hex() if b1168 else "?", b1228.hex() if b1228 else "?"))

# dump completo dos 3 trailers (0x1400 bytes cada) para diff
for name in ("trailer0", "trailer1", "trailer2"):
    addr = objs[name]
    blob = read(addr, 0x1400)
    if blob is None:
        out.write("== %s ILEGIVEL\n" % name); continue
    out.write("== %s %012X\n" % (name, addr))
    for off in range(0, 0x1400, 32):
        out.write("  +0x%04X: %s\n" % (off, blob[off:off+32].hex(" ")))

out.close()
print("snapshot %s escrito" % tag)
