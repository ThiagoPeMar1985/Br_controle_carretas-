#!/usr/bin/env python3
# scan.py — re-deriva os enderecos do plugin num eurotrucks2.exe novo.
#
# Uso:
#   python scan.py                     -> relatorio (nao escreve nada)
#   python scan.py --exe <eurotrucks2.exe>
#   python scan.py --apply             -> reescreve os VA na tabela va_sentinels[]
#   python scan.py --apply --ender     -> tambem atualiza a coluna NOVO do ENDERECOS.md
#
# O script so confia num resultado quando a assinatura aparece UMA vez no .text,
# ou quando um dos hits fica perto do endereço esperado (movimento pequeno de
# patch). Casos ambiguos e assinaturas sumidas sao listados para revisão manual.

import argparse, io, re, struct, sys

DEFAULT_EXE = r'D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe'
DEFAULT_SRC = r'..\src\windows\trailer_shared.hpp'
DEFAULT_ENDER = r'..\ENDERECOS.md'
MAX_PATCH_SHIFT = 0x100000  # hits longe demais nao sao "a mesma funcao"


def load_pe(path):
    data = open(path, 'rb').read()
    e_lfanew = struct.unpack('<I', data[0x3C:0x40])[0]
    opt = e_lfanew + 24
    image_base = struct.unpack('<Q', data[opt + 24:opt + 32])[0]
    nsec = struct.unpack('<H', data[e_lfanew + 6:e_lfanew + 8])[0]
    secs = []
    for i in range(nsec):
        o = opt + 240 + i * 40
        name = data[o:o + 8].rstrip(b'\x00').decode('ascii', 'replace')
        vsize, vaddr, rsize, raddr = struct.unpack('<IIII', data[o + 8:o + 24])
        secs.append((name, vaddr, vsize, raddr, rsize))
    return data, image_base, secs


def va2off(secs, image_base, va):
    rva = va - image_base
    for n, v, vs, ra, rs in secs:
        if v <= rva < v + vs:
            return ra + (rva - v)
    return None


def off2va(secs, image_base, fo):
    for n, v, vs, ra, rs in secs:
        if ra <= fo < ra + rs:
            return image_base + v + (fo - ra)
    return None


def count_rip_refs(exe, secs, image_base, target_va):
    # conta loads/stores RIP-relative no .text que apontam pro endereco —
    # dado global que perdeu todas as referencias provavelmente se moveu
    i, end = text_bounds(secs, image_base)
    n = 0
    for j in range(i, end - 7):
        b = exe[j:j + 3]
        if b[0] in (0x48, 0x4c) and b[1] in (0x8b, 0x89, 0x8d) and (b[2] & 0xc7) == 0x05:
            d = struct.unpack('<i', exe[j + 3:j + 7])[0]
            if off2va(secs, image_base, j) + 7 + d == target_va:
                n += 1
    return n


def parse_sentinels(src):
    pat = re.compile(
        r'//\s*(\d+)\s*-\s*([^\n]+)\n'
        r'\s*\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*'
        r'(?:\( const uint8_t\* \)"([^"]*)"|nullptr),\s*(\d+|0)\s*\}', re.S)
    out = []
    for m in pat.finditer(src):
        num, desc, old, new, sigstr, ln = m.groups()
        sig = None
        if sigstr is not None:
            sig = bytes(int(sigstr[i + 2:i + 4], 16) for i in range(0, len(sigstr), 4))
        out.append({'num': num, 'desc': desc.strip(), 'old': int(old, 16),
                    'va': int(new, 16), 'sig': sig, 'span': (m.start(), m.end())})
    return out


def text_bounds(secs, image_base):
    text = next(s for s in secs if s[0] == '.text')
    return text[3], text[3] + min(text[2], text[4])  # raddr, raddr+size


def section_bounds(secs, image_base, va):
    # faixa raw da secao onde o endereco esperado mora; 0..len = arquivo todo
    rva = va - image_base
    for n, v, vs, ra, rs in secs:
        if v <= rva < v + vs:
            return ra, ra + min(vs, rs)
    return 0, None


def scan_sig(exe, secs, image_base, sig, expected_va=None):
    if expected_va is None:
        i, end = text_bounds(secs, image_base)
    else:
        i, end = section_bounds(secs, image_base, expected_va)
    hits = []
    while True:
        j = exe.find(sig, i, end)
        if j < 0:
            break
        v = off2va(secs, image_base, j)
        if v is not None:
            hits.append(v)
        i = j + 1
    return hits


def scan_fuzzy(exe, secs, image_base, sig, expected_va):
    # assinatura com disp32 embutido muda de bytes quando a funcao anda:
    # ancora em janelas estaveis (inicio e fim — disp pode comecar no meio),
    # pontua cada candidato e aceita so um melhor claro e perto do esperado
    i, end = text_bounds(secs, image_base)
    cands = set()
    for anchor, back in ((sig[:7], 0), (sig[:8], 0), (sig[:12], 0),
                         (sig[-8:], len(sig) - 8)):
        a = i
        while True:
            j = exe.find(anchor, a, end)
            if j < 0:
                break
            base = j - back
            if base >= 0:
                v = off2va(secs, image_base, base)
                if v is not None:
                    cands.add((v, base))
            a = j + 1
    best = []
    for v, base in cands:
        if abs(v - expected_va) > MAX_PATCH_SHIFT:
            continue
        body = exe[base:base + len(sig)]
        diffs = [k for k, (x, y) in enumerate(zip(sig, body)) if x != y]
        score = 1.0 - len(diffs) / len(sig)
        # diffs em ate 3 blocos de <=4 bytes = so disp32/reloff mudou
        sizes, cur, prev = [], 0, -10
        for d in diffs:
            if d - prev > 3:
                if cur:
                    sizes.append(cur)
                cur = 0
            cur += 1
            prev = d
        if cur:
            sizes.append(cur)
        disp_shaped = len(sizes) <= 3 and max(sizes, default=0) <= 4
        best.append((score, disp_shaped, v))
    best.sort(key=lambda t: (t[1], t[0]), reverse=True)
    if not best or best[0][0] < 0.8:
        return None
    if best[0][1]:  # melhor score com cara de disp -> confiavel
        return (best[0][0], best[0][2])
    if best[0][0] >= 0.85 and (len(best) == 1 or best[1][0] < best[0][0] - 0.1):
        return (best[0][0], best[0][2])
    return None


def c_sig(sig):
    return '( const uint8_t* )"' + ''.join('\\x%02x' % b for b in sig) + '"'


# probes semanticos: enderecos que nunca tiveram sentinela, localizados
# pelo PADRAO DE COMPORTAMENTO da funcao, nao por assinatura fixa.
# '??' = byte livre. after = (hex, janela) confere bytes logo adiante.
SEMANTIC_PROBES = [
    {'name': 'phys_link', 'key': 0x140919680,
     'pattern': '48 89 4a 60 48 8b 01 48 89 51 68',
     'after': ('ff 50 78', 0x18),
     'expect': 'child+0x60=parent / parent+0x68=child / call [rax+0x78]'},
    {'name': 'joint_walk', 'key': 0x1409555F0,
     'pattern': '48 8b 49 68 48 85 c9 74 ?? 48 8b 01 ff 50 18',
     'after': None,
     'expect': 'call [rax+0x18] no filho node+0x68'},
    {'name': 'lane_obstacle_insert', 'key': 0x14094DFC0,
     'pattern': None, 'after': None,
     'expect': 'sem probe - derivar padrao do uso'},
    {'name': 'sound_vtbl', 'key': 0x142220EE60,
     'pattern': None, 'after': None,
     'expect': 'sem probe - vtable em .rdata'},
]


def parse_pattern(text):
    return [None if t == '??' else int(t, 16) for t in text.split()]


def pattern_match(exe, pos, pat):
    if pos + len(pat) > len(exe):
        return False
    return all(p is None or exe[pos + k] == p for k, p in enumerate(pat))


def scan_pattern(exe, secs, image_base, pat):
    # ancora no maior run de bytes fixos; valida o resto em cada candidato
    runs, cur, cur_at = [], [], 0
    for k, p in enumerate(pat):
        if p is None:
            if len(cur) > 0:
                runs.append((cur_at, len(cur)))
            cur = []
        else:
            if not cur:
                cur_at = k
            cur.append(p)
    if cur:
        runs.append((cur_at, len(cur)))
    anchor_at, anchor_len = max(runs, key=lambda r: r[1])
    anchor = bytes(pat[anchor_at:anchor_at + anchor_len])
    i, end = text_bounds(secs, image_base)
    hits = []
    a = i
    while True:
        j = exe.find(anchor, a, end)
        if j < 0:
            break
        base = j - anchor_at
        if base >= i and pattern_match(exe, base, pat):
            v = off2va(secs, image_base, base)
            if v is not None:
                hits.append((v, base))
        a = j + 1
    return hits


def func_start(exe, secs, image_base, fo):
    # anda pra tras ate a borda da funcao anterior (int3 ou epilogo)
    i, _ = text_bounds(secs, image_base)
    j = fo
    while j > i and exe[j - 1] == 0xCC:
        j -= 1
    while j > i and exe[j - 1] != 0xCC:
        j -= 1
    if fo - j > 0x200:
        return fo
    return j


def count_callers(exe, secs, image_base, target_va):
    i, end = text_bounds(secs, image_base)
    calls, tbl = 0, 0
    for j in range(i, end - 5):
        if exe[j] in (0xE8, 0xE9):
            d = struct.unpack('<i', exe[j + 1:j + 5])[0]
            if off2va(secs, image_base, j) + 5 + d == target_va:
                calls += 1
    needle = struct.pack('<Q', target_va)
    k = exe.find(needle)
    while k >= 0:
        tbl += 1
        k = exe.find(needle, k + 1)
    return calls, tbl


def run_probes(exe, secs, image_base):
    print('\nPROBES SEMANTICOS (sem sentinela - candidatos a verificar):')
    for p in SEMANTIC_PROBES:
        print(f"  {p['name']} (chave {p['key']:#x}): {p['expect']}")
        if p['pattern'] is None:
            print('    -> probe pendente')
            continue
        pat = parse_pattern(p['pattern'])
        hits = scan_pattern(exe, secs, image_base, pat)
        if p['after']:
            after_pat, window = parse_pattern(p['after'][0]), p['after'][1]
            hits = [(v, b) for v, b in hits
                    if any(pattern_match(exe, b + len(pat) + k, after_pat)
                           for k in range(window))]
        if not hits:
            print('    -> nenhum hit')
            continue
        for v, b in hits[:8]:
            fs = func_start(exe, secs, image_base, b)
            fva = off2va(secs, image_base, fs)
            # callers e refs sempre do entry real da funcao, nao do hit
            calls, tbl = count_callers(exe, secs, image_base, fva)
            loc = f'entry={fva:#x}' if fva != v else 'entry=hit'
            print(f'    {v:#x}  {loc}  call/jmp={calls}  refs-abs={tbl}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--src', default=DEFAULT_SRC)
    ap.add_argument('--ender', default=DEFAULT_ENDER)
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--ender-md', action='store_true')
    a = ap.parse_args()

    exe, image_base, secs = load_pe(a.exe)
    src_text = io.open(a.src, encoding='utf-8').read()
    sentinels = parse_sentinels(src_text)
    print(f'{len(sentinels)} sentinelas | exe {a.exe}\n')

    ok, moved, manual, data = [], [], [], []
    for s in sentinels:
        if s['sig'] is None:
            data.append(s)
            continue
        off = va2off(secs, image_base, s['va'])
        if off is not None and exe[off:off + len(s['sig'])] == s['sig']:
            ok.append(s)
            continue
        hits = scan_sig(exe, secs, image_base, s['sig'], s['va'])
        pick = None
        if len(hits) == 1:
            pick = hits[0]
        elif len(hits) > 1:
            near = [h for h in hits if abs(h - s['va']) <= MAX_PATCH_SHIFT]
            if len(near) == 1:
                pick = near[0]
            elif len(near) > 1:
                # patch pequeno desloca pouco: o hit mais perto do esperado,
                # desde que o segundo fique muito mais longe
                near.sort(key=lambda h: abs(h - s['va']))
                if (abs(near[0] - s['va']) <= 0x2000 and
                        abs(near[1] - s['va']) > 4 * abs(near[0] - s['va']) + 0x100):
                    pick = near[0]
        fuzzy = None
        if pick is None:
            fuzzy = scan_fuzzy(exe, secs, image_base, s['sig'], s['va'])
            if fuzzy:
                pick = fuzzy[1]
        if pick is not None:
            s['new_va'] = pick
            s['fuzzy'] = fuzzy
            moved.append(s)
        else:
            s['hits'] = hits
            manual.append(s)

    print(f'OK no endereco cadastrado: {len(ok)}')
    print(f'MOVIDAS (novo VA resolvido): {len(moved)}')
    for s in moved:
        tag = ' ~fuzzy %.0f%%' % (s['fuzzy'][0] * 100) if s.get('fuzzy') else ''
        print(f"  {s['num']:>3s} {s['desc'][:44]:44s} {s['va']:#x} -> {s['new_va']:#x}{tag}")
    print(f'DADOS (sem assinatura - refs RIP no codigo): {len(data)}')
    for s in data:
        refs = count_rip_refs(exe, secs, image_base, s['va'])
        mark = 'OK' if refs else 'SEM REFS - provavelmente moveu'
        print(f"  {s['num']:>3s} {s['desc'][:44]:44s} {s['va']:#x} refs={refs} {mark}")
    print(f'MANUAIS (assinatura ambigua ou sumida): {len(manual)}')
    for s in manual:
        hits = s['hits']
        shown = ', '.join(hex(h) for h in hits[:8]) or 'nenhum hit'
        print(f"  {s['num']:>3s} {s['desc'][:46]:46s} hits: {shown}")

    run_probes(exe, secs, image_base)

    if not a.apply or not moved:
        return

    for s in moved:
        seg = src_text[s['span'][0]:s['span'][1]]
        # reescreve so o SEGUNDO endereco do { chave, va, ... } — a chave nunca muda
        seg_new = re.sub(
            r'(\{\s*0x[0-9A-Fa-f]+,\s*)0x[0-9A-Fa-f]+',
            r'\g<1>' + hex(s['new_va']), seg, count=1)
        if s.get('fuzzy'):
            # match aproximado = disp mudou: grava os bytes reais do novo exe,
            # senao a validacao em runtime rejeita o endereco certo
            off = va2off(secs, image_base, s['new_va'])
            real = exe[off:off + len(s['sig'])]
            sig_str = '"' + ''.join('\\x%02x' % b for b in real) + '"'
            seg_new = re.sub(r'"(?:\\x[0-9a-fA-F]{2})+"',
                             lambda m: sig_str, seg_new)
        src_text = src_text[:s['span'][0]] + seg_new + src_text[s['span'][1]:]
    io.open(a.src, 'w', encoding='utf-8', newline='').write(src_text)
    print(f"\n{len(moved)} VA(s) reescritos em {a.src}")

    if a.ender_md:
        ed = io.open(a.ender, encoding='utf-8').read()
        for s in moved:
            # linha "| NNN | `ANTIGO` | `NOVO` | ..." — troca so o segundo hex
            pat_line = re.compile(
                r'(^\|\s*' + s['num'] + r'\s*\|[^|]*`0x[0-9A-Fa-f]+`[^|]*`)0x[0-9A-Fa-f]+',
                re.M)
            ed = pat_line.sub(r'\g<1>' + hex(s['new_va']), ed)
        io.open(a.ender, 'w', encoding='utf-8', newline='').write(ed)
        print(f"coluna NOVO atualizada em {a.ender}")


if __name__ == '__main__':
    sys.exit(main())
