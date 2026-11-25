# captura registros de obstaculo de lane que apontam pra dentro das carretas
# uso: python ghost_probe.py <rotulo>   (grava em ghost_probe_<rotulo>.json)
import sys, json, time
sys.path.insert(0, "tools")
from watch_live import attach, reader, u64r, find_trailers, find_containers

def elements(g, read, conts):
    els = set()
    for c in conts:
        arr = u64r(read, c + 8); cnt = u64r(read, c + 0x10)
        if not arr or not cnt or cnt > 0x4000: continue
        d = read(arr, cnt * 8)
        if not d: continue
        import struct
        for el in struct.unpack_from("<{}Q".format(cnt), d):
            if el and 0x10000 < el < 0x7FFFFFF0000: els.add(el)
    return sorted(els)

def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "snap"
    g = attach()
    read = reader(g["h"])
    trls = find_trailers(g)
    print("carretas:", ["0x%x" % t for t in trls])
    conts = find_containers(g)
    els = elements(g, read, conts)
    print("containers={} elementos={}".format(len(conts), len(els)))
    out = {"ts": time.strftime("%H:%M:%S"), "trailers": ["0x%x" % t for t in trls],
           "elements": []}
    hits = 0
    for el in els:
        evt = u64r(read, el)
        rec = el + 0x80
        recvt = u64r(read, rec)
        owner = u64r(read, el + 0x88)
        ownvt = u64r(read, owner) if owner else None
        entry = {"el": "0x%x" % el, "rec": "0x%x" % rec,
                 "elvt": "0x%x" % (evt - g["exe"]) if evt and g["exe"] <= evt < g["exe"]+0x4000000 else None,
                 "recvt": "0x%x" % (recvt - g["exe"]) if recvt and g["exe"] <= recvt < g["exe"]+0x4000000 else None,
                 "ownvt": "0x%x" % (ownvt - g["exe"]) if ownvt and g["exe"] <= ownvt < g["exe"]+0x4000000 else None,
                 "refs": []}
        # campos do registro (el+0x80 .. el+0x1C0) que apontam pra dentro de uma carreta
        import struct
        blk = read(rec, 0x140)
        if blk:
            for off in range(0, len(blk) - 7, 8):
                v = struct.unpack_from("<Q", blk, off)[0]
                for i, t in enumerate(trls):
                    if t <= v < t + 0x2000:
                        entry["refs"].append({"off": "0x%x" % off, "trailer": i,
                                              "into": "0x%x" % (v - t)})
                        hits += 1
        out["elements"].append(entry)
    fn = "tools/ghost_probe_{}.json".format(label)
    json.dump(out, open(fn, "w"), indent=1)
    print("refs pra dentro de carreta:", hits)
    for e in out["elements"]:
        if e["refs"]:
            print("  el={} recvt={} ownvt={} refs={}".format(e["el"], e["recvt"], e["ownvt"], e["refs"]))
    print("gravado", fn)

main()
