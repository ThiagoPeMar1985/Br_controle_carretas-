import tkinter as tk
import threading, queue, time, os, sys, subprocess

# Monitor de features: um painel por subsistema do plugin mostrando se o
# endereco resolve e se a feature esta viva. Verde = sano, amarelo =
# ausente/indefinido, vermelho = falhou. Rode com o jogo aberto:
#   python tools/monitor_gui.py

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from watch_live import samples

HERE = os.path.dirname(os.path.abspath(__file__))
ALERT_LOG = os.path.join(HERE, "monitor_alertas.txt")
FULL_LOG = os.path.join(HERE, "monitor_full.txt")

q = queue.Queue()
LAST_GPU = {}

def gpu_worker():
    cmd = ["nvidia-smi",
           "--query-gpu=utilization.gpu,power.draw,memory.used,memory.total,temperature.gpu",
           "--format=csv,noheader,nounits"]
    no_win = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    while True:
        try:
            out = subprocess.check_output(cmd, timeout=5, creationflags=no_win)
            p = [x.strip() for x in out.decode().strip().splitlines()[0].split(",")]
            d = {"gpu": float(p[0]), "w": float(p[1]),
                 "mem": float(p[2]), "memt": float(p[3]), "temp": float(p[4])}
            LAST_GPU.update(d)
            q.put(("gpu", d))
            time.sleep(1.5)
        except Exception:
            time.sleep(5)

def worker():
    while True:
        try:
            for kind, v in samples():
                q.put((kind, v))
        except Exception as e:
            q.put(("alert", "MONITOR FALHOU: {} - reiniciando".format(e)))
            time.sleep(2)

def fmt(v, nd=0):
    return "?" if v is None else ("{:.{d}f}".format(v, d=nd) if nd else str(int(v)))

def trl_items(st):
    trl = st.get("trl") or []
    head = st.get("trl_head")
    return [("cv" if head and i == 0 else "c{}".format(i - (1 if head else 0)), t)
            for i, t in enumerate(trl)]

def feat_steer(st):
    items = trl_items(st)
    trl = st.get("trl") or []
    txt = ""
    if items:
        txt = " ".join("{}={}".format(
            n, ("ok " + ("{:+.2f}".format(t["steer_ang"]) if t.get("steer_ang") is not None else "ang?"))
            if t.get("steer_data") else "sem dados") for n, t in items)
    else:
        txt = "sem carreta na memoria"
    if st.get("str") is not None: txt += "  |  str={}/s".format(int(st["str"]))
    ok = any(t.get("steer_data") for t in trl)
    return txt, "#1e7d32" if ok else ("#8a6d00" if trl else "#555555")

def feat_lift(st):
    items = trl_items(st)
    if not items: return "sem carreta na memoria", "#555555"
    txt = " ".join("{}={}".format(
        n, "mask{:X}/st{:X}".format(t["lift_mask"] or 0, t["lift_state"] or 0)
        if t.get("lift_mask") is not None else "sem ctrl") for n, t in items)
    ok = any(t.get("lift_mask") is not None for _, t in items)
    return txt, "#1e7d32" if ok else "#8a6d00"

def feat_susp(st):
    items = trl_items(st)
    if not items: return "sem carreta na memoria", "#555555"
    bad = False; parts = []
    for n, t in items:
        s = t.get("susp")
        if s is None or not t.get("susp_ok"): bad = True
        parts.append("{}={}".format(n, "{:.2f}".format(s) if s is not None else "falha"))
    txt = " ".join(parts)
    if st.get("suspw") is not None: txt += "  |  writes={}".format(int(st["suspw"]))
    return txt, "#c0392b" if bad else "#1e7d32"

def feat_detach(st):
    items = trl_items(st)
    txt = "jnt={} det={} orph={}".format(fmt(st.get("jnt")), fmt(st.get("det")), fmt(st.get("orph")))
    if items:
        txt += "  |  " + " ".join("{}={}".format(n, "eng" if t.get("joint") else "solta") for n, t in items)
    return txt, "#c0392b" if st.get("orph") else "#1e7d32"

def feat_truck(st):
    txt = "hb={}/s jw={}/s ws={}/s".format(fmt(st.get("hb")), fmt(st.get("jw")), fmt(st.get("ws")))
    bad = st.get("hb") is not None and st["hb"] < 100
    return txt, "#c0392b" if bad else "#1e7d32"

def feat_lane(st):
    el = st.get("el")
    if el is None: el = st.get("el_mem")
    txt = "el={} mortos={} carimb={} maxno={} ins={}/s".format(
        fmt(el), st.get("mortos", 0), st.get("carimb", 0), st.get("maxno", 0), fmt(st.get("ins")))
    bad = bool(st.get("mortos")) or (st.get("ins") or 0) > 50
    return txt, "#c0392b" if bad else "#1e7d32"

def feat_gpu(st):
    g = LAST_GPU
    if not g: return "sem dados da placa", "#555555"
    fps = st.get("fps_live") or st.get("fps")
    txt = "{:.0f}% | {:.0f}W | {:.1f}/{:.0f}GB | {:.0f}C".format(
        g["gpu"], g["w"], g["mem"] / 1024.0, g["memt"] / 1024.0, g["temp"])
    if g["gpu"] >= 90:
        return txt + " | placa no limite", "#1e7d32"
    if fps is not None and fps < 40 and g["gpu"] < 50:
        return txt + " | CPU segurando a placa", "#c0392b"
    return txt + " | folga", "#e6b400"

FEATURES = [
    ("Placa de vídeo", feat_gpu),
    ("Eixos direcionais", feat_steer),
    ("Erguer eixo", feat_lift),
    ("Suspensão", feat_susp),
    ("Desatrelamento", feat_detach),
    ("Cavalo / física", feat_truck),
    ("Lane obstáculos", feat_lane),
]

class Monitor(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Monitor da carreta")
        self.configure(bg="#101418")
        self.attributes("-topmost", True)
        self.geometry("560x640")

        self.banner = tk.Label(self, text="ESPERANDO O JOGO...",
                               font=("Consolas", 20, "bold"),
                               bg="#8a6d00", fg="white", pady=12)
        self.banner.pack(fill="x")

        self.sub = tk.Label(self, text="", font=("Consolas", 10),
                            bg="#101418", fg="#8a9aa8", anchor="w")
        self.sub.pack(fill="x", padx=10)

        fr = tk.Frame(self, bg="#101418")
        fr.pack(fill="x", padx=10, pady=6)
        self.feat_dot = {}; self.feat_txt = {}
        for name, _ in FEATURES:
            row = tk.Frame(fr, bg="#101418")
            row.pack(fill="x", pady=2)
            d = tk.Label(row, text="●", fg="#555555", bg="#101418", font=("Consolas", 13))
            d.pack(side="left")
            tk.Label(row, text=name, fg="#c8d4dc", bg="#101418",
                     font=("Consolas", 10, "bold"), width=17, anchor="w").pack(side="left", padx=(4, 6))
            v = tk.Label(row, text="", fg="#e8eef2", bg="#101418",
                         font=("Consolas", 9), anchor="w", justify="left")
            v.pack(side="left", fill="x", expand=True)
            self.feat_dot[name] = d; self.feat_txt[name] = v

        tk.Label(self, text="FPS", font=("Consolas", 11, "bold"),
                 bg="#101418", fg="#c8d4dc", anchor="w").pack(fill="x", padx=10, pady=(4, 0))
        self.fps_cv = tk.Canvas(self, height=64, bg="#0a0d10",
                                highlightthickness=0)
        self.fps_cv.pack(fill="x", padx=10)
        self.fps_lbl = tk.Label(self, text="", font=("Consolas", 9),
                                bg="#101418", fg="#8a9aa8", anchor="w")
        self.fps_lbl.pack(fill="x", padx=10, pady=(0, 4))
        self.fps_hist = []

        tk.Label(self, text="EVENTOS  (vermelho = problema)", font=("Consolas", 11, "bold"),
                 bg="#101418", fg="#c0392b", anchor="w").pack(fill="x", padx=10, pady=(6, 0))
        self.alerts = tk.Listbox(self, font=("Consolas", 9),
                                 bg="#1a0e0e", fg="#ff6b5e",
                                 selectbackground="#3a1515",
                                 activestyle="none", height=12)
        self.alerts.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        self.ok_since = time.time()
        self.af = open(ALERT_LOG, "a", encoding="utf-8")
        self.ff = open(FULL_LOG, "a", encoding="utf-8")
        self.ff.write("\n===== sessao {} =====\n".format(time.strftime("%d/%m %H:%M:%S")))
        threading.Thread(target=worker, daemon=True).start()
        threading.Thread(target=gpu_worker, daemon=True).start()
        self.after(100, self.drain)

    def ts(self):
        return time.strftime("%H:%M:%S")

    def drain(self):
        try:
            while True:
                kind, v = q.get_nowait()
                if kind == "alert":
                    self.alert(str(v))
                elif kind == "info":
                    self.event(str(v), "#8a9aa8")
                elif kind == "gpu":
                    self.ff.write("{} gpu {}\n".format(self.ts(), v)); self.ff.flush()
                else:
                    self.status(v)
        except queue.Empty:
            pass
        self.after(100, self.drain)

    def status(self, st):
        if st.get("waiting"):
            self.banner.config(text="ESPERANDO O JOGO...", bg="#8a6d00")
            return
        for name, fn in FEATURES:
            txt, color = fn(st)
            self.feat_txt[name].config(text=txt)
            self.feat_dot[name].config(fg=color)
        if self.banner["bg"] != "#c0392b":
            self.banner.config(text="OK", bg="#1e7d32")
            self.sub.config(text="fps {}   limpo há {}".format(
                fmt(st.get("fps_live") or st.get("fps")),
                time.strftime("%H:%M:%S", time.gmtime(time.time() - self.ok_since))))
        self.draw_fps(st)
        self.ff.write("{} status {}\n".format(self.ts(), st))
        self.ff.flush()

    def draw_fps(self, st):
        fps = st.get("fps_live") or st.get("fps")
        if fps is not None:
            self.fps_hist.append(fps)
            if len(self.fps_hist) > 150: self.fps_hist.pop(0)
        cv = self.fps_cv
        cv.delete("all")
        w = cv.winfo_width() or 540
        h = 64
        cap = 90.0
        for ref, col in ((60, "#2a4a2a"), (30, "#4a2a2a")):
            y = h - min(ref, cap) / cap * h
            cv.create_line(0, y, w, y, fill=col, dash=(3, 4))
        n = len(self.fps_hist)
        if n >= 2:
            step = w / 149.0
            for i in range(1, n):
                a, b = self.fps_hist[i - 1], self.fps_hist[i]
                x0 = w - (n - i) * step
                x1 = w - (n - i - 1) * step
                y0 = h - min(a, cap) / cap * h
                y1 = h - min(b, cap) / cap * h
                col = "#4caf50" if b >= 55 else ("#e6b400" if b >= 30 else "#ff5a4e")
                cv.create_line(x0, y0, x1, y1, fill=col, width=2)
        if fps is not None:
            frame_ms = 1000.0 / fps if fps > 0 else 0
            parts = ["{:.0f} fps".format(fps), "frame {:.0f}ms".format(frame_ms)]
            if st.get("worst") is not None:
                parts.append("pior {}ms".format(int(st["worst"])))
            if st.get("plug_ms") is not None:
                parts.append("plugin ~{:.1f}ms/f".format(st["plug_ms"] / fps if fps > 0 else 0))
            if st.get("ins_native_ms") and st["ins_native_ms"] > 0.5:
                parts.append("jogo ins {:.0f}ms/s".format(st["ins_native_ms"]))
            if st.get("pp_prod") is not None:
                parts.append("prod {:.0f}ms".format(st["pp_prod"]))
            if st.get("pp_in") is not None:
                parts.append("entrega {:.0f}ms".format(st["pp_in"]))
            self.fps_lbl.config(text="   |   ".join(parts))

    def event(self, msg, color):
        line = "{}  {}".format(self.ts(), msg)
        self.alerts.insert("end", line)
        self.alerts.itemconfig("end", fg=color)
        self.alerts.see("end")
        if self.alerts.size() > 300:
            self.alerts.delete(0)
        self.ff.write(line + "\n"); self.ff.flush()

    def alert(self, msg):
        self.event(msg, "#ff6b5e")
        self.banner.config(text="PROBLEMA!", bg="#c0392b")
        self.sub.config(text=msg[:90])
        self.ok_since = time.time()
        self.af.write("{}  {}\n".format(self.ts(), msg)); self.af.flush()
        self.after(4000, self.maybe_recover)

    def maybe_recover(self):
        if time.time() - self.ok_since >= 3.9 and self.banner["bg"] == "#c0392b":
            self.banner.config(text="OK (recuperou)", bg="#1e7d32")

Monitor().mainloop()
