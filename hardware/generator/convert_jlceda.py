"""Convert hardware/kicad to a JLCEDA Pro (.eprj3) project with easyeda/kicad-to-easyeda-eprj3, then fix up silkscreen.

Usage: python convert_jlceda.py <path to kicad-to-easyeda-eprj3 checkout>
"""
import json
import os
import shutil
import subprocess
import sys
import zipfile

import parts
import write_kicad

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD = os.path.normpath(os.path.join(HERE, "..", "kicad"))
# .eprj3 projects take their name from both the folder and the index file, so they must match.
OUT = os.path.normpath(os.path.join(HERE, "..", "jlceda", write_kicad.PROJECT))
MIL = 39.3700787


def mirror_path(path):
    out = []
    numeric = 0
    for v in path:
        if isinstance(v, list):
            if v and v[0] == "CIRCLE":
                v = [v[0], -v[1]] + v[2:]
            out.append(v)
        elif isinstance(v, (int, float)):
            out.append(-v if numeric % 2 == 0 else v)
            numeric += 1
        else:
            out.append(v)
    return out


def bottom_docs(lines):
    """FOOTPRINT docs the converter built from a back-side instance (pads on layer 2)."""
    docs, doc = set(), None
    for line in lines:
        if "||" not in line:
            continue
        head_s, body_s = line.split("||", 1)
        head = json.loads(head_s)
        if head.get("type") == "DOCHEAD":
            doc = json.loads(body_s[:-1])
        elif head.get("type") == "PAD" and doc and doc["docType"] == "FOOTPRINT" and json.loads(body_s[:-1])["layerId"] == 2:
            docs.add(doc["uuid"])
    return docs


def unflip_footprint_record(kind, body):
    """Library footprints are top-view; EasyEDA mirrors them when the instance sits on the bottom layer."""
    if kind == "PAD":
        body["centerX"] = -body["centerX"]
        body["padAngle"] = (360 - body["padAngle"]) % 360
        if body["layerId"] == 2:
            body["layerId"] = 1
    elif kind == "POLY":
        body["path"] = mirror_path(body["path"])
        if body["layerId"] == 4:
            body["layerId"] = 3
    elif kind == "PART":
        x1, y1, x2, y2 = body["BBOX"]
        body["BBOX"] = [-x2, y1, -x1, y2]
    return body


def fix_pcb(path):
    comps = {c["ref"]: c for c in parts.COMPONENTS}
    out = []
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    flipped = bottom_docs(lines)
    doc = None
    for line in lines:
        if "||" not in line:
            out.append(line)
            continue
        head_s, body_s = line.split("||", 1)
        head = json.loads(head_s)
        if head.get("type") == "DOCHEAD":
            doc = json.loads(body_s[:-1])
        if doc and doc.get("docType") == "FOOTPRINT" and doc["uuid"] in flipped and head.get("type") in ("PAD", "POLY", "PART"):
            body = unflip_footprint_record(head["type"], json.loads(body_s[:-1]))
            out.append(head_s + "||" + json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "|")
            continue
        if head.get("type") not in ("STRING", "ATTR") or not body_s.endswith("|"):
            out.append(line)
            continue
        body = json.loads(body_s[:-1])
        if head["type"] == "STRING" and body.get("layerId") == 4:
            body["mirror"] = True
        elif head["type"] == "ATTR" and body.get("key") == "Designator" and body.get("value") in comps:
            c = comps[body["value"]]
            fp = parts.FOOTPRINTS[c["fp"]]
            rx, ry = fp["ref_at"]
            back = c.get("side", "F") == "B"
            if back:
                rx = -rx
            x, y = c["at"]
            body["x"] = round((write_kicad.OX + x + rx) * MIL, 3)
            body["y"] = round(-(write_kicad.OY + y + ry) * MIL, 3)
            body["origin"] = "CENTER_MIDDLE"
            body["fontSize"] = 32
            body["strokeWidth"] = 5
            if back:
                body["layerId"] = 4
                body["mirror"] = True
            if fp.get("hide_ref"):
                body["valueVisible"] = False
        else:
            out.append(line)
            continue
        out.append(head_s + "||" + json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "|")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out))


def main():
    k2e = sys.argv[1]
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    subprocess.run(["node", os.path.join(k2e, "scripts", "convert-kicad.js"), "convert", KICAD, OUT,
                    "--project-name", write_kicad.PROJECT], check=True)
    fix_pcb(os.path.join(OUT, "pcb", "PCB1.epcb2"))
    print("JLCEDA project:", os.path.join(OUT, write_kicad.PROJECT + ".eprj3"))
    # Folder project packed for transfer: unzip, then File > Open the .eprj3 (the Import menu only takes .epro).
    archive = os.path.join(OUT, "..", "..", write_kicad.PROJECT + "_eprj3.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(OUT):
            for name in sorted(files):
                full = os.path.join(root, name)
                zf.write(full, os.path.relpath(full, os.path.dirname(OUT)).replace(os.sep, "/"))
    print("JLCEDA zip:", os.path.normpath(archive))


if __name__ == "__main__":
    main()
