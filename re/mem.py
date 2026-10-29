import ctypes, struct
from ctypes import wintypes

k32 = ctypes.windll.kernel32
TH32CS_SNAPMODULE = 0x8
PROCESS_ALL = 0x1F0FFF

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                ("szModule", ctypes.c_char*256), ("szExePath", ctypes.c_char*260)]

def open_proc(pid):
    return k32.OpenProcess(PROCESS_ALL, False, pid)

def mod_base(pid, name=b'eurotrucks2.exe'):
    s = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | 0x10, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    base = None
    if k32.Module32First(s, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == name:
                base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
                break
            if not k32.Module32Next(s, ctypes.byref(me)): break
    k32.CloseHandle(s)
    return base

def rpm(h, addr, n):
    buf = (ctypes.c_char * n)()
    read = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(read)):
        return None
    return buf.raw[:read.value]

def u64(buf, off): return struct.unpack_from('<Q', buf, off)[0]
