import ctypes, struct, subprocess

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
print("pid=%s" % pid)
h = k32.OpenProcess(0x0410, False, pid)
print("handle=%s" % h)

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

def u64(addr):
    b = read(addr, 8)
    return struct.unpack("<Q", b)[0] if b and len(b) == 8 else 0

truck = 0x2C8A1414818
trs = [0x2C8A1417518, 0x2C8A14187E8, 0x2C8A1419AB8]
print("truck_towed=%X" % u64(truck + 0x1580))
for i, a in enumerate(trs):
    print("t%d: joint=%X parent=%X slave=%X rec=%X parked=%s reg=%s" % (
        i, u64(a + 0xFE0), u64(a + 0x1080), u64(a + 0x1058), u64(a + 0x1158),
        (read(a + 0x1168, 1) or b"?").hex(), (read(a + 0x1228, 1) or b"?").hex()))
