import ctypes, sys

k32 = ctypes.windll.kernel32
pid = int(sys.argv[1]) if len(sys.argv) > 1 else 19356
T0 = 0x1C579716D28

h = k32.OpenProcess(0x0010 | 0x0400 | 0x0020, False, pid)  # +VM_WRITE
if not h:
    print("OpenProcess falhou", ctypes.get_last_error()); sys.exit(1)

for off, name in ((0x1168, "parked"), (0x1228, "parked_registered")):
    buf = ctypes.create_string_buffer(b"\x00", 1)
    got = ctypes.c_size_t()
    ok = k32.WriteProcessMemory(h, ctypes.c_void_p(T0 + off), buf, 1, ctypes.byref(got))
    print("zerado %s (+0x%X): %s" % (name, off, "ok" if ok else "FALHOU"))
