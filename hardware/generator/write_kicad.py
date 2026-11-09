"""Write the KiCad 7 project (schematic, PCB, libraries) and the JLC BOM."""
import csv
import json
import os
import uuid

import parts
import symbols

OX, OY = 100.0, 100.0
LIB = "wiretft"
PROJECT = "wireTFT_board"
NS = uuid.UUID("6f0c8f43-7d0e-4c55-9a53-0e1f1b6c2a11")


def uid(*key):
    return str(uuid.uuid5(NS, "/".join(str(k) for k in key)))


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def f(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


FONT = "(effects (font (size 1.27 1.27)))"
HIDE = "(effects (font (size 1.27 1.27)) hide)"


# ------------------------------------------------------------------ symbols
def lib_symbol(sym, full_name):
    out = [f"(symbol {q(full_name)}"]
    if not sym["show_pin_numbers"]:
        out.append("(pin_numbers hide)")
    out.append("(pin_names (offset 0.254)" + ("" if sym["show_pin_names"] else " hide") + ")")
    out.append("(in_bom yes) (on_board yes)")
    rx, ry = sym["ref_at"]
    vx, vy = sym["val_at"]
    out.append(f'(property "Reference" {q(sym["ref"])} (at {f(rx)} {f(ry)} 0) (effects (font (size 1.27 1.27)) (justify left)))')
    out.append(f'(property "Value" {q(sym["name"])} (at {f(vx)} {f(vy)} 0) (effects (font (size 1.27 1.27)) (justify left)))')
    out.append(f'(property "Footprint" "" (at 0 0 0) {HIDE})')
    out.append(f'(property "Datasheet" "" (at 0 0 0) {HIDE})')
    base = sym["name"]
    out.append(f"(symbol {q(base + '_0_1')}")
    for g in sym["gfx"]:
        if g[0] == "rect":
            out.append(f"(rectangle (start {f(g[1])} {f(g[2])}) (end {f(g[3])} {f(g[4])}) (stroke (width 0.254) (type default)) (fill (type background)))")
        elif g[0] == "poly":
            pts = " ".join(f"(xy {f(x)} {f(y)})" for x, y in g[1])
            out.append(f"(polyline (pts {pts}) (stroke (width {f(g[2])}) (type default)) (fill (type none)))")
        elif g[0] == "circle":
            out.append(f"(circle (center {f(g[1])} {f(g[2])}) (radius {f(g[3])}) (stroke (width 0.254) (type default)) (fill (type none)))")
    out.append(")")
    out.append(f"(symbol {q(base + '_1_1')}")
    for p in sym["pins"]:
        out.append(f'(pin {p["etype"]} line (at {f(p["x"])} {f(p["y"])} {p["rot"]}) (length {f(p["length"])}) '
                   f'(name {q(p["name"])} {FONT}) (number {q(p["num"])} {FONT}))')
    out.append(")")
    out.append(")")
    return "\n".join(out)


def pin_outward(rot):
    return {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[rot]


def write_schematic(path, sheet_uuid):
    lines = ["(kicad_sch (version 20230121) (generator eeschema)", f"(uuid {q(sheet_uuid)})", '(paper "A3")',
             '(title_block (title "wireTFT desktop frame / music display - carrier board") (date "2026-10-26") (rev "1.0") '
             '(company "wireTFT") (comment 1 "ESP32-WROOM-32 on the back, 2.4in ST7789 FPC on the front") '
             '(comment 2 "USB-C 5V -> AMS1117 3.3V, CH340C UART with auto-download, RST/BOOT buttons"))']
    lines.append("(lib_symbols")
    for key, sym in symbols.SYMBOLS.items():
        lines.append(lib_symbol(sym, f"{LIB}:{sym['name']}"))
    lines.append(")")
    body = []
    for c in parts.COMPONENTS:
        sym = symbols.SYMBOLS[c["sym"]]
        X, Y = symbols.SHEET_POS[c["ref"]]
        rx, ry = sym["ref_at"]
        vx, vy = sym["val_at"]
        body.append(f'(symbol (lib_id {q(LIB + ":" + sym["name"])}) (at {f(X)} {f(Y)} 0) (unit 1) (in_bom {"no" if c.get("no_bom") else "yes"}) '
                    f'(on_board yes) (dnp no) (uuid {q(uid("sym", c["ref"]))})')
        body.append(f'(property "Reference" {q(c["ref"])} (at {f(X + rx)} {f(Y - ry)} 0) (effects (font (size 1.27 1.27)) (justify left)))')
        body.append(f'(property "Value" {q(c["value"])} (at {f(X + vx)} {f(Y - vy)} 0) (effects (font (size 1.27 1.27)) (justify left)))')
        body.append(f'(property "Footprint" {q(LIB + ":" + c["fp"])} (at {f(X)} {f(Y)} 0) {HIDE})')
        body.append(f'(property "Datasheet" "" (at {f(X)} {f(Y)} 0) {HIDE})')
        if c.get("lcsc"):
            body.append(f'(property "LCSC" {q(c["lcsc"])} (at {f(X)} {f(Y)} 0) {HIDE})')
        for p in sym["pins"]:
            body.append(f'(pin {q(p["num"])} (uuid {q(uid("pin", c["ref"], p["num"]))}))')
        body.append(f'(instances (project {q(PROJECT)} (path {q("/" + sheet_uuid)} (reference {q(c["ref"])}) (unit 1))))')
        body.append(")")
        for p in sym["pins"]:
            ex, ey = X + p["x"], Y - p["y"]
            net = c["nets"].get(p["num"])
            if not net:
                body.append(f'(no_connect (at {f(ex)} {f(ey)}) (uuid {q(uid("nc", c["ref"], p["num"]))}))')
                continue
            dx, dy = pin_outward(p["rot"])
            wx, wy = ex + dx * 2.54, ey + dy * 2.54
            body.append(f'(wire (pts (xy {f(ex)} {f(ey)}) (xy {f(wx)} {f(wy)})) (stroke (width 0) (type default)) '
                        f'(uuid {q(uid("w", c["ref"], p["num"]))}))')
            angle, just = {(-1, 0): (180, "right"), (1, 0): (0, "left"), (0, -1): (90, "left"), (0, 1): (270, "right")}[(dx, dy)]
            body.append(f'(label {q(net)} (at {f(wx)} {f(wy)} {angle}) (fields_autoplaced) '
                        f'(effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid {q(uid("lbl", c["ref"], p["num"]))}))')
    notes = [
        (30.48, 30.48, "wireTFT carrier board - signal map follows wireTFT.ino:"),
        (30.48, 35.56, "SPI: SCK=GPIO18  MOSI=GPIO23  CS=GPIO21  DC=GPIO27  RST=GPIO33  BL=GPIO32 (active high)"),
        (30.48, 40.64, "USB-C VBUS -> B5819W -> +5V -> AMS1117-3.3 -> +3V3 for WROOM-32, CH340C and the panel"),
        (30.48, 45.72, "CH340C TXD/RXD -> U0RXD/U0TXD, DTR/RTS -> EN/IO0 via MMBT3904 pair (esptool auto reset)"),
        (30.48, 50.8, "EN: 10k pull-up + 1uF; IO0: 10k pull-up; SW1 = RST, SW2 = BOOT; strapping pins IO2/5/12/15 left open"),
    ]
    for x, y, t in notes:
        body.append(f'(text {q(t)} (at {f(x)} {f(y)} 0) (effects (font (size 1.778 1.778)) (justify left bottom)) (uuid {q(uid("txt", t))}))')
    lines += body
    lines.append(f'(sheet_instances (path "/" (page "1")))')
    lines.append(")")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def write_symbol_lib(path):
    lines = ["(kicad_symbol_lib (version 20220914) (generator kicad_symbol_editor)"]
    for sym in symbols.SYMBOLS.values():
        lines.append(lib_symbol(sym, sym["name"]))
    lines.append(")")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


# ------------------------------------------------------------------ footprints
def flip_layer(layer):
    if layer.startswith("F."):
        return "B." + layer[2:]
    if layer.startswith("B."):
        return "F." + layer[2:]
    return layer


def fp_items(fp, ref, value, netmap, net_ids, key, side="F"):
    back = side == "B"
    sx = -1 if back else 1
    L = flip_layer if back else (lambda l: l)
    out = []
    rx, ry = fp["ref_at"]
    rx *= sx
    rl = L(fp.get("ref_layer", "F.SilkS"))
    mirror = " (justify mirror)" if rl.startswith("B.") else ""
    hide = " hide" if fp.get("hide_ref") else ""
    out.append(f'(fp_text reference {q(ref)} (at {f(rx)} {f(ry)}) (layer {q(rl)}){hide} '
               f'(effects (font (size 0.8 0.8) (thickness 0.12)){mirror}) (tstamp {q(uid(key, "ref"))}))')
    out.append(f'(fp_text value {q(value)} (at 0 0) (layer {q(L("F.Fab"))}) hide (effects (font (size 0.8 0.8) (thickness 0.12)){mirror}) '
               f'(tstamp {q(uid(key, "val"))}))')
    for n, g in enumerate(fp["gfx"]):
        if g[0] == "line":
            _, x1, y1, x2, y2, layer, w = g
            out.append(f'(fp_line (start {f(sx * x1)} {f(y1)}) (end {f(sx * x2)} {f(y2)}) (stroke (width {f(w)}) (type solid)) '
                       f'(layer {q(L(layer))}) (tstamp {q(uid(key, "g", n))}))')
        elif g[0] == "circle":
            _, cx, cy, r, layer, w = g
            out.append(f'(fp_circle (center {f(sx * cx)} {f(cy)}) (end {f(sx * cx + r)} {f(cy)}) (stroke (width {f(w)}) (type solid)) '
                       f'(fill none) (layer {q(L(layer))}) (tstamp {q(uid(key, "g", n))}))')
    for n, p in enumerate(fp["pads"]):
        at = f"(at {f(sx * p['x'])} {f(p['y'])}{' ' + str(p['angle']) if p.get('angle') else ''})"
        layers = " ".join(q(L(l)) for l in p["layers"])
        s = f'(pad {q(p["num"])} {p["kind"]} {p["shape"]} {at} (size {f(p["w"])} {f(p["h"])})'
        if p.get("oval_drill"):
            s += f" (drill oval {f(p['oval_drill'][0])} {f(p['oval_drill'][1])})"
        elif p.get("drill"):
            s += f" (drill {f(p['drill'])})"
        s += f" (layers {layers})"
        if p["shape"] == "roundrect":
            s += " (roundrect_rratio 0.25)"
        net = netmap.get(p["num"]) if p["num"] else None
        if net and net_ids is not None:
            s += f" (net {net_ids[net]} {q(net)})"
        s += f' (tstamp {q(uid(key, "pad", n))}))'
        out.append(s)
    return out


def attr_of(fp):
    a = fp.get("attr", "smd")
    if a == "exclude":
        return "(attr exclude_from_pos_files exclude_from_bom)"
    return f"(attr {a})"


def write_footprint_lib(dirpath):
    os.makedirs(dirpath, exist_ok=True)
    for name, fp in parts.FOOTPRINTS.items():
        lines = [f'(footprint {q(name)} (version 20221018) (generator pcbnew)', '(layer "F.Cu")', attr_of(fp)]
        lines += fp_items(fp, "REF**", name, {}, None, ("lib", name))
        lines.append(")")
        with open(os.path.join(dirpath, name + ".kicad_mod"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")


# ------------------------------------------------------------------ board
LAYER_TABLE = """(layers
(0 "F.Cu" signal)
(31 "B.Cu" signal)
(32 "B.Adhes" user "B.Adhesive")
(33 "F.Adhes" user "F.Adhesive")
(34 "B.Paste" user)
(35 "F.Paste" user)
(36 "B.SilkS" user "B.Silkscreen")
(37 "F.SilkS" user "F.Silkscreen")
(38 "B.Mask" user)
(39 "F.Mask" user)
(40 "Dwgs.User" user "User.Drawings")
(41 "Cmts.User" user "User.Comments")
(42 "Eco1.User" user "User.Eco1")
(43 "Eco2.User" user "User.Eco2")
(44 "Edge.Cuts" user)
(45 "Margin" user)
(46 "B.CrtYd" user "B.Courtyard")
(47 "F.CrtYd" user "F.Courtyard")
(48 "B.Fab" user)
(49 "F.Fab" user)
)"""

SETUP = """(setup
(pad_to_mask_clearance 0.05)
(allow_soldermask_bridges_in_footprints no)
(pcbplotparams (layerselection 0x00010fc_ffffffff) (plot_on_all_layers_selection 0x0000000_00000000)
(disableapertmacros false) (usegerberextensions false) (usegerberattributes true) (usegerberadvancedattributes true)
(creategerberjobfile true) (dashed_line_dash_ratio 12.000000) (dashed_line_gap_ratio 3.000000) (svgprecision 4)
(plotframeref false) (viasonmask false) (mode 1) (useauxorigin false) (hpglpennumber 1) (hpglpenspeed 20)
(hpglpendiameter 15.000000) (dxfpolygonmode true) (dxfimperialunits true) (dxfusepcbnewfont true) (psnegative false)
(psa4output false) (plotreference true) (plotvalue true) (plotinvisibletext false) (sketchpadsonfab false)
(subtractmaskfromsilk false) (outputformat 1) (mirror false) (drillshape 0) (scaleselection 1) (outputdirectory "gerber/"))
)"""


def zone_poly(x1, y1, x2, y2):
    return (f"(polygon (pts (xy {f(OX + x1)} {f(OY + y1)}) (xy {f(OX + x2)} {f(OY + y1)}) "
            f"(xy {f(OX + x2)} {f(OY + y2)}) (xy {f(OX + x1)} {f(OY + y2)})))")


def silk_text(text, x, y, layer, size=1.0, key=None, angle=0):
    mirror = " (justify mirror)" if layer.startswith("B.") else ""
    rot = f" {angle}" if angle else ""
    return (f'(gr_text {q(text)} (at {f(OX + x)} {f(OY + y)}{rot}) (layer {q(layer)}) (tstamp {q(uid("t", key or text))}) '
            f'(effects (font (size {f(size)} {f(size)}) (thickness {f(size * 0.15)})){mirror}))')


def board_graphics():
    out = []
    W, H = parts.BOARD_W, parts.BOARD_H
    out.append(f'(gr_rect (start {f(OX)} {f(OY)}) (end {f(OX + W)} {f(OY + H)}) (stroke (width 0.1) (type default)) '
               f'(fill none) (layer "Edge.Cuts") (tstamp {q(uid("edge"))}))')
    x1, y1, x2, y2 = parts.SCREEN
    L = 3.0
    for n, (cx, cy, sx, sy) in enumerate([(x1, y1, 1, 1), (x2, y1, -1, 1), (x1, y2, 1, -1), (x2, y2, -1, -1)]):
        for m, (ex, ey) in enumerate([(cx + sx * L, cy), (cx, cy + sy * L)]):
            out.append(f'(gr_line (start {f(OX + cx)} {f(OY + cy)}) (end {f(OX + ex)} {f(OY + ey)}) '
                       f'(stroke (width 0.15) (type default)) (layer "F.SilkS") (tstamp {q(uid("scr", n, m))}))')
    out.append(f'(gr_rect (start {f(OX + x1)} {f(OY + y1)}) (end {f(OX + x2)} {f(OY + y2)}) (stroke (width 0.1) (type dash)) '
               f'(fill none) (layer "Dwgs.User") (tstamp {q(uid("scr_doc"))}))')
    texts = [
        ("wireTFT carrier v1.1", parts.CX, 40.0, "F.SilkS", 1.5),
        ("2.4in ST7789 panel area (stick here)", parts.CX, 44.0, "F.SilkS", 1.0),
        ("USB-C 5V / UART", parts.CX + 0.05, parts.BOARD_H - 9.2, "F.SilkS", 0.8),
        ("ANTENNA - keep clear", parts.CX, 3.2, "F.SilkS", 1.0),
        ("ANTENNA - keep clear", parts.CX, 3.2, "B.SilkS", 1.0),
        ("ESP32-WROOM-32", parts.CX, 31.0, "B.SilkS", 1.2),
        ("RST", 47.0, 17.2, "B.SilkS", 1.0),
        ("BOOT", 9.6, 28.2, "B.SilkS", 1.0),
        ("wireTFT carrier v1.1", parts.CX, 40.0, "B.SilkS", 1.2),
    ]
    for t, x, y, layer, size in texts:
        out.append(silk_text(t, x, y, layer, size, key=t + layer))
    return out


def write_pcb(path, router):
    nets = sorted({t[0] for t in router.tracks} | {v[0] for v in router.vias} | set(router.nets))
    net_ids = {n: i + 1 for i, n in enumerate(nets)}
    lines = ["(kicad_pcb (version 20221018) (generator pcbnew)", "(general (thickness 1.6))", '(paper "A4")',
             '(title_block (title "wireTFT carrier board") (date "2026-10-26") (rev "1.0") (company "wireTFT"))',
             LAYER_TABLE, SETUP, '(net 0 "")']
    for n in nets:
        lines.append(f"(net {net_ids[n]} {q(n)})")
    for c in parts.COMPONENTS:
        fp = parts.FOOTPRINTS[c["fp"]]
        x, y = c["at"]
        key = ("pcb", c["ref"])
        side = c.get("side", "F")
        lines.append(f'(footprint {q(LIB + ":" + c["fp"])} (layer {q("B.Cu" if side == "B" else "F.Cu")}) '
                     f'(tstamp {q(uid(*key))}) (at {f(OX + x)} {f(OY + y)})')
        if c.get("desc"):
            lines.append(f'(descr {q(c["desc"])})')
        if c.get("lcsc"):
            lines.append(f'(property "LCSC" {q(c["lcsc"])})')
        lines.append(attr_of(fp))
        lines += fp_items(fp, c["ref"], c["value"], c["nets"], net_ids, key, side)
        lines.append(")")
    for k, (net, layer, x1, y1, x2, y2, w) in enumerate(router.tracks):
        lines.append(f'(segment (start {f(OX + x1)} {f(OY + y1)}) (end {f(OX + x2)} {f(OY + y2)}) (width {f(w)}) '
                     f'(layer {q("F.Cu" if layer == 0 else "B.Cu")}) (net {net_ids[net]}) (tstamp {q(uid("seg", k))}))')
    for k, (net, x, y, d, drill) in enumerate(router.vias):
        lines.append(f'(via (at {f(OX + x)} {f(OY + y)}) (size {f(d)}) (drill {f(drill)}) (layers "F.Cu" "B.Cu") '
                     f'(net {net_ids[net]}) (tstamp {q(uid("via", k))}))')
    m = parts.EDGE_CLEARANCE
    for layer in ("F.Cu", "B.Cu"):
        lines.append(f'(zone (net {net_ids["GND"]}) (net_name "GND") (layer {q(layer)}) (tstamp {q(uid("zone", layer))}) '
                     f'(name {q("GND_" + layer[0])}) (hatch edge 0.5) (connect_pads (clearance 0.3)) (min_thickness 0.25) '
                     f'(filled_areas_thickness no) (fill yes (thermal_gap 0.4) (thermal_bridge_width 0.4)) '
                     + zone_poly(m, m, parts.BOARD_W - m, parts.BOARD_H - m) + ")")
    zones = [("ANTENNA_KEEPOUT", parts.KEEPOUTS[0], "not_allowed"), ("MODULE_BODY", parts.KEEPOUTS[1], "allowed")]
    for name, (kx1, ky1, kx2, ky2, layers), pour in zones:
        for layer in layers:
            lines.append(f'(zone (net 0) (net_name "") (layer {q(layer)}) (tstamp {q(uid("ko", name, layer))}) (name {q(name)}) '
                         f'(hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25) (filled_areas_thickness no) '
                         f'(keepout (tracks not_allowed) (vias not_allowed) (pads allowed) (copperpour {pour}) (footprints allowed)) '
                         f'(fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) ' + zone_poly(kx1, ky1, kx2, ky2) + ")")
    lines += board_graphics()
    lines.append(")")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def write_project(dirpath, sheet_uuid):
    pro = {
        "board": {"design_settings": {"rules": {"min_clearance": parts.CLEARANCE, "min_track_width": 0.2,
                                                "min_via_diameter": 0.6, "min_through_hole_diameter": 0.3,
                                                "min_copper_edge_clearance": parts.EDGE_CLEARANCE}}},
        "meta": {"filename": PROJECT + ".kicad_pro", "version": 1},
        "sheets": [[sheet_uuid, ""]],
    }
    with open(os.path.join(dirpath, PROJECT + ".kicad_pro"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(pro, fh, indent=2)
    with open(os.path.join(dirpath, "fp-lib-table"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f'(fp_lib_table\n  (lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.pretty")(options "")(descr ""))\n)\n')
    with open(os.path.join(dirpath, "sym-lib-table"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f'(sym_lib_table\n  (lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.kicad_sym")(options "")(descr ""))\n)\n')


def write_bom(path):
    groups = {}
    for c in parts.COMPONENTS:
        if c.get("no_bom"):
            continue
        key = (c["value"], c["fp"], c["lcsc"], c["desc"] if c["ref"] in ("U1", "FPC1") else "")
        groups.setdefault(key, []).append(c["ref"])
    with open(path, "w", encoding="utf-8-sig", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["Comment", "Designator", "Footprint", "JLCPCB Part #", "Quantity", "Note"])
        for (value, fp, lcsc, note), refs in sorted(groups.items(), key=lambda kv: kv[1][0]):
            w.writerow([value, ",".join(refs), fp, lcsc, len(refs), note])


def write_all(outdir, router):
    os.makedirs(outdir, exist_ok=True)
    sheet_uuid = uid("sheet")
    write_schematic(os.path.join(outdir, PROJECT + ".kicad_sch"), sheet_uuid)
    write_pcb(os.path.join(outdir, PROJECT + ".kicad_pcb"), router)
    write_project(outdir, sheet_uuid)
    write_symbol_lib(os.path.join(outdir, LIB + ".kicad_sym"))
    write_footprint_lib(os.path.join(outdir, LIB + ".pretty"))
