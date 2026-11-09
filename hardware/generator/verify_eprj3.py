"""Re-read the converted .epcb2 and compare pad positions/nets, tracks and vias with the generator data."""
import json
import math
import os
import sys

import parts
import write_kicad
from geometry import board_pads

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.normpath(os.path.join(HERE, "..", "jlceda", "wireTFT_board", "pcb", "PCB1.epcb2"))
MIL = 39.3700787


def to_mm(x, y):
    return x / MIL - write_kicad.OX, -y / MIL - write_kicad.OY


def records(path):
    doc = None
    for line in open(path, encoding="utf-8").read().split("\n"):
        if "||" not in line:
            continue
        h, b = line.split("||", 1)
        head, body = json.loads(h), json.loads(b[:-1]) if b.endswith("|") else None
        if head.get("type") == "DOCHEAD":
            doc = body
        yield doc, head, body


def main():
    fp_pads, comps, comp_fp, comp_ref, pad_net, lines, vias = {}, {}, {}, {}, {}, [], []
    for doc, head, body in records(PCB):
        t = head.get("type")
        if t == "PAD" and doc["docType"] == "FOOTPRINT":
            fp_pads.setdefault(doc["uuid"], {})[head["id"]] = body
        elif t == "COMPONENT" and doc["docType"] == "PCB":
            comps[head["id"]] = body
            comp_ref[head["id"]] = body["attrs"]["Unique ID"]
        elif t == "ATTR" and doc["docType"] == "PCB" and body.get("key") == "Footprint":
            comp_fp[body["parentId"]] = body["value"]
        elif t == "PAD_NET":
            _, cid, num, pid = json.loads(head["id"])
            pad_net[(cid, pid)] = body["padNet"]
        elif t == "LINE" and doc["docType"] == "PCB" and body.get("layerId") in (1, 2) and body.get("netName"):
            lines.append(body)
        elif t == "VIA":
            vias.append(body)
    got = []
    layer_bad = []
    for cid, c in comps.items():
        a = math.radians(c["angle"])
        back = c["layerId"] == 2
        for pid, p in fp_pads[comp_fp[cid]].items():
            px = -p["centerX"] if back else p["centerX"]  # top-view library footprint mirrored onto the bottom
            x = c["x"] + px * math.cos(a) - p["centerY"] * math.sin(a)
            y = c["y"] + px * math.sin(a) + p["centerY"] * math.cos(a)
            if p["layerId"] == 2:
                layer_bad.append((comp_ref[cid], p["num"]))
            src = next(cc for cc in parts.COMPONENTS if cc["ref"] == comp_ref[cid])
            if back != (src.get("side", "F") == "B"):
                layer_bad.append((comp_ref[cid], "component side"))
            mx, my = to_mm(x, y)
            w, h = p["defaultPad"]["width"] / MIL, p["defaultPad"]["height"] / MIL
            if round(p["padAngle"]) % 180 == 90:
                w, h = h, w
            got.append((comp_ref[cid], p["num"], round(mx, 3), round(my, 3), round(w, 3), round(h, 3),
                        pad_net.get((cid, pid), ""), p["layerId"], p["hole"]))
    want = []
    for c, p, prim in board_pads():
        w, h = (p["h"], p["w"]) if p.get("angle") in (90, 270) else (p["w"], p["h"])
        net = prim.net if isinstance(prim.net, str) else ""
        want.append((c["ref"], p["num"], round(c["at"][0] + p["x"], 3), round(c["at"][1] + p["y"], 3), round(w, 3), round(h, 3), net))
    gs = sorted(g[:7] for g in got)
    ws = sorted(want)
    bad = [(a, b) for a, b in zip(gs, ws) if a[:2] != b[:2] or any(abs(a[i] - b[i]) > 0.002 for i in (2, 3, 4, 5)) or a[6] != b[6]]
    print(f"pads: eprj3={len(gs)} source={len(ws)} mismatches={len(bad) + abs(len(gs) - len(ws))} "
          f"bottom-side records left in footprint docs / side errors: {sorted(set(layer_bad))}")
    bad += [(x, x) for x in layer_bad]
    for a, b in bad[:20]:
        print("  got", a, "\n  want", b)
    slots = [g for g in got if g[0] == "J1" and g[1].startswith("S")]
    for s in slots:
        hole = s[8]
        print(f"  J1 {s[1]} pad {s[4]}x{s[5]} mm, hole {hole['width'] / MIL:.2f}x{hole['height'] / MIL:.2f} (pad angle applies to both)")
    print(f"tracks: eprj3={len(lines)} vias: eprj3={len(vias)}")
    sys.exit(1 if bad or len(gs) != len(ws) else 0)


if __name__ == "__main__":
    main()
