import ctypes, struct, subprocess, time, re, os, sys
from ctypes import wintypes
from collections import Counter

# Motor do monitor ao vivo: samples() gera ("status", dict) e ("alert", msg)
# a cada segundo. Usado pelo modo console (este arquivo) e pela janela
# (monitor_gui.py). Memoria + log de saude do plugin, sem tocar no jogo.

LOG = os.path.expanduser(r"~\Documents\Euro Truck Simulator 2\br_carretas_debug.log")
INTERVAL = 1.0

k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

class MI(ctypes.Structure):
    _fields_ = [("b", ctypes.c_void_p), ("s", wintypes.DWORD), ("e", ctypes.c_void_p)]

def find_pid():
    n = wintypes.DWORD(4096)
    pids = (wintypes.DWORD * 4096)()
    if not psapi.EnumProcesses(ctypes.byref(pids), ctypes.sizeof(pids), ctypes.byref(n)):
        return None
    buf = ctypes.create_unicode_buffer(1024)
    for pid in pids[: n.value // 4]:
        if not pid: continue
        ph = k32.OpenProcess(0x1000, False, pid)
        if not ph: continue
        sz = wintypes.DWORD(1024)
        ok = k32.QueryFullProcessImageNameW(ph, 0, buf, ctypes.byref(sz))
        k32.CloseHandle(ph)
        if ok and buf.value.lower().endswith("eurotrucks2.exe"):
            return pid
    return None

def attach():
    pid = find_pid()
    if not pid: return None
    h = k32.OpenProcess(0x1F0FFF, False, pid)
    if not h: return None
    mods = (ctypes.c_void_p * 1024)(); need = wintypes.DWORD()
    psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need))
    exe_base = dll_base = dll_size = 0
    for i in range(need.value // 8):
        n = ctypes.create_string_buffer(260)
        psapi.GetModuleFileNameExA(h, ctypes.c_void_p(mods[i]), n, 260)
        p = n.value.decode().lower(); info = MI()
        psapi.GetModuleInformation(h, ctypes.c_void_p(mods[i]), ctypes.byref(info), 24)
        if p.endswith("eurotrucks2.exe"): exe_base = info.b
        if "br_controle_de_carretas" in p: dll_base, dll_size = info.b, info.s
    if not dll_base:
        k32.CloseHandle(h); return None
    return {"h": h, "exe": exe_base, "dll": dll_base, "dllsz": dll_size, "pid": pid}

def reader(h):
    def read(a, s):
        b = ctypes.create_string_buffer(s); g = ctypes.c_size_t()
        ok = k32.ReadProcessMemory(h, ctypes.c_void_p(a), b, s, ctypes.byref(g))
        return b.raw[:g.value] if ok else None
    return read

def find_containers(g):
    read = reader(g["h"])
    def u64(a):
        b = read(a, 8); return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None
    def heapish(p): return p and 0x10000 < p < 0x7FFFFFF0000 and not (g["exe"] <= p < g["exe"] + 0x4000000)
    region = read(g["dll"], g["dllsz"])
    if not region: return []
    conts = []
    for i in range(0, g["dllsz"] - 8, 8):
        v = struct.unpack_from("<Q", region, i)[0]
        if not heapish(v): continue
        arr = u64(v + 8); cnt = u64(v + 0x10)
        if not heapish(arr) or not cnt or cnt > 0x4000: continue
        d = read(arr, 8)
        if d and heapish(struct.unpack("<Q", d)[0]): conts.append(v)
    return conts

def is_trailer_ptr(g, read, p):
    if not p or not (0x10000 < p < 0x7FFFFFF0000): return False
    vt = struct.unpack("<Q", read(p, 8) or b"\0" * 8)[0]
    if not (g["exe"] <= vt < g["exe"] + 0x4000000): return False
    susp = f32r(read, p + 0x4D4)
    if susp is None or not (0.3 <= susp <= 3.0): return False
    for off in (0x6C8, 0x6C0, 0xFE0, 0x1080):
        q = u64r(read, p + off)
        if q and 0x10000 < q < 0x7FFFFFF0000: return True
    return False

def find_trailers(g):
    read = reader(g["h"])
    region = read(g["dll"], g["dllsz"])
    if not region: return []
    found = []
    for i in range(0, g["dllsz"] - 8, 8):
        v = struct.unpack_from("<Q", region, i)[0]
        if v in found: continue
        if is_trailer_ptr(g, read, v): found.append(v)
    return found[:6]

def u64r(read, a):
    b = read(a, 8); return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

def f32r(read, a):
    b = read(a, 4); return struct.unpack("<f", b)[0] if b and len(b) == 4 else None

def u32r(read, a):
    b = read(a, 4); return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

def trailer_info(g, read, t):
    info = {"ptr": t, "joint": bool(u64r(read, t + 0xFE0)),
            "vt": (u64r(read, t) or 0) - g["exe"],
            "parent": u64r(read, t + 0x1080) or 0}
    susp = f32r(read, t + 0x4D4)
    info["susp"] = susp
    info["susp_ok"] = susp is not None and 0.3 <= susp <= 3.0
    sd_ptr = u64r(read, t + 0x6C0)
    info["steer_data"] = bool(sd_ptr)
    ang = f32r(read, t + 0x6B8)
    info["steer_ang"] = ang if ang is not None and -2 <= ang <= 2 else None
    ctrl = u64r(read, t + 0x6C8)
    if ctrl:
        info["lift_mask"] = u32r(read, ctrl + 0x38)
        info["lift_state"] = u32r(read, ctrl + 0x3C)
        ws = u64r(read, t + 0x200)
        info["lift_flags_arr"] = bool(ws and u64r(read, ws + 0x5C8))
    else:
        info["lift_mask"] = info["lift_state"] = info["lift_flags_arr"] = None
    return info

def census(g, conts):
    read = reader(g["h"])
    def u64(a):
        b = read(a, 8); return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None
    total = dead = stamped = nullown = 0
    per_node_max = 0
    owners = set()
    for c in conts:
        arr = u64(c + 8); cnt = u64(c + 0x10)
        if not arr or not cnt or cnt > 0x4000: continue
        per_node_max = max(per_node_max, cnt)
        d = read(arr, cnt * 8)
        if not d: continue
        for el in struct.unpack_from("<{}Q".format(cnt), d):
            if el == 0: continue
            total += 1
            vt = u64(el + 0)
            if not vt or not (g["exe"] <= vt < g["exe"] + 0x4000000):
                dead += 1; continue
            own = u64(el + 0x88)
            if not own:
                nullown += 1
            else:
                ovt = u64(own + 0)
                if ovt and g["dll"] <= ovt < g["dll"] + g["dllsz"]: stamped += 1
                else: owners.add(own)
    return total, dead, stamped, nullown, per_node_max, len(owners)

def num(pat, line):
    m = re.search(pat, line)
    return float(m.group(1)) if m else None

def fps_mem(g, read, prev, cands):
    region = read(g["dll"], g["dllsz"])
    if not region: return
    cands.clear()
    for i in range(0, g["dllsz"] - 8, 8):
        v = struct.unpack_from("<Q", region, i)[0]
        a = g["dll"] + i
        old = prev.get(a)
        prev[a] = v
        if old is not None and v > old and 5 <= v - old <= 500:
            cands[a] = v - old

def samples():
    g = None; conts = []; logpos = 0; last_scan = 0
    trls = []; last_tscan = 0
    prev_el = None; el_grow_streak = 0
    sd_streak = mortos_streak = dono_streak = 0
    prev_q = {}; cands = {}; fps_addr = None; last_fps = None
    yield ("info", "esperando eurotrucks2.exe...")
    while True:
        if g is None:
            g = attach()
            if g:
                conts = find_containers(g); last_scan = time.time()
                trls = find_trailers(g); last_tscan = time.time()
                yield ("info", "conectado pid={} ({} containers de lane)".format(g["pid"], len(conts)))
                try: logpos = os.path.getsize(LOG)
                except OSError: logpos = 0
            else:
                yield ("status", {"waiting": True})
                time.sleep(2); continue

        try:
            if os.path.getsize(LOG) < logpos:
                logpos = 0
            with open(LOG, "r", encoding="utf-8", errors="replace") as f:
                f.seek(logpos); new = f.read(); logpos = f.tell()
        except OSError:
            new = ""
        st = {"waiting": False}
        for line in new.splitlines():
            low = line.lower()
            if "purecall" in low or "falha" in low or "sim stall" in low or "exception" in low:
                yield ("alert", "LOG: " + line.strip()[:160])
            if "presente:" in low:
                st["pp_prod"] = num(r"prod=([\d.]+)", line)
                st["pp_in"] = num(r"entreg=([\d.]+)", line)
            if "health:" in low:
                st["fps"] = num(r"fps=([\d.]+)", line)
                st["worst"] = num(r"worst=(\d+)ms", line)
                st["el"] = num(r"lane el=(\d+)", line)
                st["ins"] = num(r"ins=([\d.]+)/s", line)
                st["ins_native_ms"] = num(r"ins=[\d.]+/s [\d.]+ms\+([\d.]+)ms", line)
                hsec = line.split(" h:", 1)
                if len(hsec) == 2:
                    st["plug_ms"] = sum(float(m) for m in re.findall(r"\w+=[\d.]+/([\d.]+)", hsec[1]))
                st["tfff"] = num(r"t:0xfff=(\d+)", line)
                st["sd"] = num(r"sd=(\d+)", line)
                st["jnt"] = num(r"jnt=(\d+)", line)
                st["det"] = num(r"det=(\d+)", line)
                st["orph"] = num(r"orph=(\d+)", line)
                st["jw"] = num(r"jw=([\d.]+)/s", line)
                st["ws"] = num(r"ws=([\d.]+)/s", line)
                st["str"] = num(r"str=([\d.]+)/s", line)
                st["suspw"] = num(r"suspw\+(\d+)", line)
                st["hb"] = num(r"hb=([\d.]+)/s", line)

        rc = wintypes.DWORD()
        if not k32.GetExitCodeProcess(g["h"], ctypes.byref(rc)) or rc.value != 259:
            yield ("alert", "PROCESSO MORREU - veja o fim do log e o Event Viewer")
            g = None; conts = []; trls = []; prev_el = None; el_grow_streak = 0
            sd_streak = mortos_streak = dono_streak = 0
            prev_q = {}; cands = {}; fps_addr = None; last_fps = None
            time.sleep(2); continue

        if conts:
            st["el_mem"], st["mortos"], st["carimb"], st["semdono"], st["maxno"], st["donos"] = census(g, conts)

        r2 = reader(g["h"])
        fps_mem(g, r2, prev_q, cands)
        if st.get("fps"):
            last_fps = st["fps"]
            fps_addr = None
        if fps_addr not in cands and last_fps is not None:
            best = min(cands.items(), key=lambda kv: abs(kv[1] - last_fps), default=None)
            if best and abs(best[1] - last_fps) <= max(8.0, last_fps * 0.4):
                fps_addr = best[0]
        if fps_addr in cands:
            st["fps_live"] = float(cands[fps_addr])
        trls = [t for t in trls if is_trailer_ptr(g, r2, t)]
        if time.time() - last_tscan > 15:
            found = find_trailers(g)
            for t in found:
                if t not in trls: trls.append(t)
            last_tscan = time.time()
        infos = [trailer_info(g, r2, t) for t in trls[:4]]
        vts = Counter(i["vt"] for i in infos)
        trl_vt = vts.most_common(1)[0][0] if vts else 0
        ordered = ([i for i in infos if i["vt"] != trl_vt] +
                   sorted((i for i in infos if i["vt"] == trl_vt), key=lambda i: i["ptr"]))
        st["trl"] = ordered
        st["trl_head"] = len(ordered) != len([i for i in infos if i["vt"] == trl_vt])
        now = time.time()
        if (not conts and now - last_scan > 15) or now - last_scan > 60:
            found = find_containers(g)
            if len(found) != len(conts):
                yield ("info", "containers de lane: {} -> {}".format(len(conts), len(found)))
                conts = found
            last_scan = now

        if st.get("ins") and st["ins"] > 50:
            yield ("alert", "TEMPESTADE de re-registro: ins={}/s t:0xfff={}".format(
                int(st["ins"]), int(st.get("tfff") or 0)))
        if st.get("sd"):
            sd_streak = sd_streak + 1
            if st["sd"] >= 2 or sd_streak >= 3:
                yield ("alert", "SIM STALL persistente: sd={} {} ciclos".format(int(st["sd"]), sd_streak))
            else:
                yield ("info", "sim engasgou um tick (sd={})".format(int(st["sd"])))
        else:
            sd_streak = 0
        if st.get("mortos"):
            mortos_streak = mortos_streak + 1
            if st["mortos"] >= 3 or mortos_streak >= 4:
                yield ("alert", "{} elemento(s) MORTOS ha {}s (jogo nao limpa)".format(st["mortos"], mortos_streak))
            else:
                yield ("info", "{} elemento(s) morto(s) transitorio(s)".format(st["mortos"]))
        else:
            mortos_streak = 0
        if st.get("semdono"):
            dono_streak = dono_streak + 1
            if st["semdono"] >= 3 or dono_streak >= 3:
                yield ("alert", "{} elemento(s) com dono nulo ha {}s".format(st["semdono"], dono_streak))
            else:
                yield ("info", "{} elemento(s) com dono nulo transitorio(s)".format(st["semdono"]))
        else:
            dono_streak = 0
        if st.get("tfff") and st["tfff"] > 30:
            yield ("alert", "{} elementos tipo=0xFFF sem ref acumulando".format(int(st["tfff"])))
        el = st.get("el") or st.get("el_mem")
        if el is not None:
            el_grow_streak = el_grow_streak + 1 if prev_el is not None and el > prev_el + 15 else 0
            if el_grow_streak >= 3:
                yield ("alert", "lane crescendo sem parar: el={}".format(int(el)))
            prev_el = el

        yield ("status", st)
        time.sleep(INTERVAL)

def watch():
    out = open(sys.argv[1], "w", encoding="utf-8") if len(sys.argv) > 1 else None
    def say(msg):
        line = time.strftime("%H:%M:%S ") + msg
        print(line, flush=True)
        if out: out.write(line + "\n"); out.flush()
    for kind, v in samples():
        if kind == "alert":
            say("!! " + str(v))
        elif kind == "info":
            say("-- " + str(v))
        else:
            if v.get("waiting"): continue
            s = "ok fps={} el={} mortos={} carimb={} semdono={} maxno={} donos={}".format(
                int(v["fps"]) if v.get("fps") else "?", v.get("el") or v.get("el_mem") or "?",
                v.get("mortos", 0), v.get("carimb", 0), v.get("semdono", 0),
                v.get("maxno", 0), v.get("donos", 0))
            if v.get("ins") is not None: s += " ins={}/s".format(int(v["ins"]))
            if v.get("jnt") is not None:
                s += " jnt={} det={} orph={}".format(int(v["jnt"]), int(v.get("det") or 0), int(v.get("orph") or 0))
            say(s)

if __name__ == "__main__":
    watch()
