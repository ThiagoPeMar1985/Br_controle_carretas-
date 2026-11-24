import ctypes, struct, subprocess, sys

k32 = ctypes.windll.kernel32
out = subprocess.check_output('tasklist /FI "IMAGENAME eq eurotrucks2.exe" /FO CSV', shell=True).decode()
pid = None
for line in out.splitlines():
    if line.lower().startswith('"eurotrucks2'):
        pid = int(line.split('","')[1]); break
if pid is None:
    print("jogo nao esta rodando"); sys.exit(1)
print("PID", pid)

h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
if not h:
    print("OpenProcess falhou", ctypes.get_last_error()); sys.exit(1)

def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value] if ok else None

BASE = 0x140000000
# dump descriptor region: scale_x desc @ 0x142D859A8, scale_y desc ~+0x140
va = 0x142D859A8
blob = read(va, 0x2C0)
if blob is None:
    print("leitura falhou"); sys.exit(1)

for i in range(0, 0x2C0, 16):
    row = blob[i:i+16]
    asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
    floats = ' '.join(f"{struct.unpack('<f', row[j:j+4])[0]:.3g}" for j in range(0, 16, 4))
    print(f"{va+i:x}: {row.hex(' ')}  {asc}  |{floats}|")
