import ctypes, struct, subprocess, time, os

k32 = ctypes.windll.kernel32
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
DUMP = r"C:\Users\PC\Documents\Euro Truck Simulator 2\br_carretas_memdump.txt"
OUT  = r"C:\Users\PC\Documents\Euro Truck Simulator 2\br_mem_timeline.txt"

def acha_pid():
    out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
    for line in out.splitlines():
        if line.lower().startswith('"eurotrucks2'):
            return int(line.split('","')[1])
    return None

def le_enderecos():
    if not os.path.exists(DUMP):
        return None
    objs = {}
    for line in open(DUMP, encoding="utf-8", errors="ignore"):
        if line.startswith("== "):
            parts = line.split()
            if len(parts) == 3:
                objs[parts[1]] = int(parts[2], 16)
    return objs if "trailer0" in objs else None

h = None
objs = None
errs = 0

def log(msg):
    with open(OUT, "a", encoding="utf-8") as f:
        f.write("%s %s\n" % (time.strftime("%H:%M:%S"), msg))

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

def u64(addr):
    b = read(addr, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else 0

def estado(o):
    s = {}
    tr = o["truck"]
    s["truck_towed"] = u64(tr + 0x1580)
    b = read(tr + 0x14B8, 4); s["truck_state"] = b.hex() if b else "?"
    for i in range(3):
        a = o["trailer%d" % i]
        s["t%d_joint" % i] = u64(a + 0xFE0)
        s["t%d_parent" % i] = u64(a + 0x1080)
        s["t%d_slave" % i] = u64(a + 0x1058)
        s["t%d_rec" % i] = u64(a + 0x1158)
        b = read(a + 0x1168, 1); s["t%d_parked" % i] = b.hex() if b else "?"
        b = read(a + 0x1228, 1); s["t%d_reg" % i] = b.hex() if b else "?"
    return s

def fmt(s):
    return " ".join("%s=%X" % (k, v) if isinstance(v, int) else "%s=%s" % (k, v) for k, v in s.items())

print("monitor v2 iniciado - aguardando jogo + mundo...")
prev = None
while True:
    probe = read(objs["truck"], 4) if (h and objs) else None
    if h is None or probe is None:
        try:
            if h: k32.CloseHandle(h)
        except Exception:
            pass
        h = None
        pid = acha_pid()
        if pid:
            h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
            log("monitor: conectado pid=%d" % pid)
            objs = le_enderecos()
        prev = None
        time.sleep(2)
        continue
    novo = le_enderecos()
    if novo != objs:
        objs = novo
        prev = None
        if objs:
            log("monitor: enderecos novos carregados")
    if objs is None:
        time.sleep(2)
        continue
    try:
        s = estado(objs)
        errs = 0
    except Exception:
        errs += 1
        if errs == 10:
            log("monitor: 10 falhas de leitura - aguardando mundo novo")
            prev = None
        time.sleep(1)
        continue
    if s != prev:
        with open(OUT, "a", encoding="utf-8") as f:
            f.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt(s)))
        prev = s
    time.sleep(0.5)
