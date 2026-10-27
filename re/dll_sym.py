"""Map RVAs in our DLL to functions via .pdata."""
import struct, re, sys

DLL = r'D:\plugins para ets\BR_controle_de_carretas\build\Release\br_controle_de_carretas.dll'
data = open(DLL, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3C)[0]
numsec = struct.unpack_from('<H', data, pe + 6)[0]
optsize = struct.unpack_from('<H', data, pe + 20)[0]
soff = pe + 24 + optsize
secs = {}
for i in range(numsec):
    s = soff + i * 40
    name = data[s:s+8].split(b'\0')[0].decode('latin1')
    vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', data, s + 8)
    secs[name] = (vaddr, vsize, roff, rsize)

# optional header: data dirs start at pe+24+96 (x64)
dd = pe + 24 + 96
exc_rva, exc_size = struct.unpack_from('<II', data, dd + 3 * 8)

def rva2off(rva):
    for name, (vaddr, vsize, roff, rsize) in secs.items():
        if vaddr <= rva < vaddr + rsize:
            return roff + (rva - vaddr)
    return None

# .pdata: 12-byte RUNTIME_FUNCTION entries (begin, end, unwind)
pdata_off = rva2off(exc_rva)
funcs = []
for off in range(pdata_off, pdata_off + exc_size, 12):
    b, e, u = struct.unpack_from('<III', data, off)
    funcs.append((b, e))
funcs.sort()

def find(rva):
    for b, e in funcs:
        if b <= rva < e:
            return b
    return None

import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

targets = [int(x, 16) for x in sys.argv[1:]]
for t in targets:
    fb = find(t)
    print(f'RVA {t:#x}: func start {fb:#x}' if fb else f'RVA {t:#x}: NO FUNC')
    if fb:
        off = rva2off(fb)
        code = data[off:off + (t - fb) + 64]
        # disasm last ~30 instructions up to the call site
        ins = list(md.disasm(code, fb))
        for insn in ins[max(0, len(ins) - 12):]:
            marker = ' <<<' if insn.address <= t < insn.address + insn.size else ''
            print(f'  {insn.address:#x}: {insn.mnemonic} {insn.op_str}{marker}')
