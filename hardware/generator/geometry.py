"""Copper primitives and exact clearance math shared by the router and the DRC."""
import math

import parts

LAYERS = ("F.Cu", "B.Cu")


class Prim:
    """Axis-aligned rectangle (kind 'rect') or capsule/segment with radius (kind 'cap')."""
    __slots__ = ("kind", "a", "net", "layers", "owner")

    def __init__(self, kind, a, net, layers, owner):
        self.kind, self.a, self.net, self.layers, self.owner = kind, a, net, layers, owner

    def bbox(self):
        if self.kind == "rect":
            cx, cy, hx, hy = self.a
            return cx - hx, cy - hy, cx + hx, cy + hy
        x1, y1, x2, y2, r = self.a
        return min(x1, x2) - r, min(y1, y2) - r, max(x1, x2) + r, max(y1, y2) + r


def pad_layers(p, side="F"):
    if p["kind"] == "smd":
        return (0,) if side == "F" else (1,)
    return (0, 1)


def placed_pad(c, p):
    """Pad dict in board orientation: back-side parts are mirrored in X."""
    if c.get("side", "F") != "B":
        return p
    q = dict(p)
    q["x"] = -p["x"]
    q["layers"] = tuple(l.replace("F.", "B.") for l in p["layers"])
    return q


def pad_prim(p, ox, oy, net, owner, side="F"):
    x, y = ox + p["x"], oy + p["y"]
    w, h = p["w"], p["h"]
    if p.get("angle", 0) in (90, 270):
        w, h = h, w
    layers = pad_layers(p, side)
    if p["kind"] == "np_thru_hole":
        return Prim("cap", (x, y, x, y, p["drill"] / 2), -1, layers, owner)
    if p["shape"] in ("rect", "roundrect"):
        return Prim("rect", (x, y, w / 2, h / 2), net, layers, owner)
    if p["shape"] == "circle":
        return Prim("cap", (x, y, x, y, w / 2), net, layers, owner)
    if w >= h:
        d = (w - h) / 2
        return Prim("cap", (x - d, y, x + d, y, h / 2), net, layers, owner)
    d = (h - w) / 2
    return Prim("cap", (x, y - d, x, y + d, w / 2), net, layers, owner)


def board_pads():
    """All pads as (component, pad dict, Prim)."""
    out = []
    for c in parts.COMPONENTS:
        fp = parts.FOOTPRINTS[c["fp"]]
        ox, oy = c["at"]
        side = c.get("side", "F")
        for p0 in fp["pads"]:
            p = placed_pad(c, p0)
            net = c["nets"].get(p["num"]) if p["num"] else None
            out.append((c, p, pad_prim(p, ox, oy, net, (c["ref"], p["num"]), side)))
    return out


def pt_seg(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / L))
    return math.hypot(px - x1 - t * dx, py - y1 - t * dy)


def seg_seg(a, b):
    x1, y1, x2, y2 = a
    x3, y3, x4, y4 = b
    if segments_intersect(a, b):
        return 0.0
    return min(pt_seg(x1, y1, *b), pt_seg(x2, y2, *b), pt_seg(x3, y3, *a), pt_seg(x4, y4, *a))


def segments_intersect(a, b):
    x1, y1, x2, y2 = a
    x3, y3, x4, y4 = b

    def orient(ax, ay, bx, by, cx, cy):
        v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        return 0 if abs(v) < 1e-12 else (1 if v > 0 else -1)

    o1, o2 = orient(x1, y1, x2, y2, x3, y3), orient(x1, y1, x2, y2, x4, y4)
    o3, o4 = orient(x3, y3, x4, y4, x1, y1), orient(x3, y3, x4, y4, x2, y2)
    if o1 != o2 and o3 != o4:
        return True

    def on(ax, ay, bx, by, cx, cy):
        return min(ax, bx) - 1e-12 <= cx <= max(ax, bx) + 1e-12 and min(ay, by) - 1e-12 <= cy <= max(ay, by) + 1e-12

    return ((o1 == 0 and on(x1, y1, x2, y2, x3, y3)) or (o2 == 0 and on(x1, y1, x2, y2, x4, y4)) or
            (o3 == 0 and on(x3, y3, x4, y4, x1, y1)) or (o4 == 0 and on(x3, y3, x4, y4, x2, y2)))


def pt_rect(px, py, cx, cy, hx, hy):
    dx = max(abs(px - cx) - hx, 0.0)
    dy = max(abs(py - cy) - hy, 0.0)
    return math.hypot(dx, dy)


def seg_rect(seg, rect):
    x1, y1, x2, y2 = seg
    cx, cy, hx, hy = rect
    # Liang-Barsky clip: any overlap means distance 0
    t0, t1 = 0.0, 1.0
    dx, dy = x2 - x1, y2 - y1
    inside = True
    for p, q in ((-dx, x1 - (cx - hx)), (dx, (cx + hx) - x1), (-dy, y1 - (cy - hy)), (dy, (cy + hy) - y1)):
        if p == 0:
            if q < 0:
                inside = False
                break
        else:
            t = q / p
            if p < 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
            if t0 > t1:
                inside = False
                break
    if inside:
        return 0.0
    corners = [(cx - hx, cy - hy), (cx + hx, cy - hy), (cx + hx, cy + hy), (cx - hx, cy + hy)]
    d = min(pt_rect(x1, y1, *rect), pt_rect(x2, y2, *rect))
    for px, py in corners:
        d = min(d, pt_seg(px, py, *seg))
    return d


def prim_dist(p, q):
    if p.kind == "rect" and q.kind == "rect":
        (cx1, cy1, hx1, hy1), (cx2, cy2, hx2, hy2) = p.a, q.a
        dx = max(abs(cx1 - cx2) - hx1 - hx2, 0.0)
        dy = max(abs(cy1 - cy2) - hy1 - hy2, 0.0)
        return math.hypot(dx, dy)
    if p.kind == "rect":
        p, q = q, p
    if q.kind == "rect":
        return max(seg_rect(p.a[:4], q.a) - p.a[4], 0.0)
    return max(seg_seg(p.a[:4], q.a[:4]) - p.a[4] - q.a[4], 0.0)


def edge_dist(p):
    x1, y1, x2, y2 = p.bbox()
    return min(x1, y1, parts.BOARD_W - x2, parts.BOARD_H - y2)
