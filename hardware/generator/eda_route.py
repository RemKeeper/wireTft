"""Route selected nets on a copper snapshot exported from EasyEDA (board mm, Y down) and emit tracks/vias in mil.

Usage: python eda_route.py snapshot.json out.json NET [NET ...]
"""
import json
import sys

import parts
import router
from geometry import Prim

FINE_REFS = {"J1"}  # 0.5 mm pitch receptacle pads cap the connection width


def pad_prim(p, nid_of):
    layers = {1: (0,), 2: (1,), 12: (0, 1)}.get(p["layer"], (0,))
    net = p["net"] or None
    if p["poly"]:
        x1, y1, x2, y2 = p["poly"]
        a = ((x1 + x2) / 2, (y1 + y2) / 2, abs(x2 - x1) / 2, abs(y2 - y1) / 2)
        return Prim("rect", a, net, layers, (p["ref"], p["num"]))
    w, h = p["w"], p["h"]
    if round(p["rot"] or 0) % 180 == 90:
        w, h = h, w
    x, y = p["x"], p["y"]
    if p["shape"] == "ELLIPSE":
        return Prim("cap", (x, y, x, y, w / 2), net, layers, (p["ref"], p["num"]))
    if p["shape"] == "OVAL":
        if w >= h:
            d = (w - h) / 2
            return Prim("cap", (x - d, y, x + d, y, h / 2), net, layers, (p["ref"], p["num"]))
        d = (h - w) / 2
        return Prim("cap", (x, y - d, x, y + d, w / 2), net, layers, (p["ref"], p["num"]))
    return Prim("rect", (x, y, w / 2, h / 2), net, layers, (p["ref"], p["num"]))


class SnapRouter(router.Router):
    def __init__(self, snap):
        self.snap = snap
        nets = sorted({p["net"] for p in snap["pads"] if p["net"]} | {l[0] for l in snap["lines"] if l[0]})
        self.nets = nets
        self.net_id = {n: i + 1 for i, n in enumerate(nets)}
        self.id_net = {i: n for n, i in self.net_id.items()}
        self.edge = router.edge_map()
        self.static = router.DistMap()
        self.pads = []
        uid = -2
        for p in snap["pads"]:
            prim = pad_prim(p, self.net_id)
            self.pads.append(({"ref": p["ref"]}, {"num": p["num"]}, prim))
            if prim.net:
                nid = self.net_id[prim.net]
            else:
                nid, uid = uid, uid - 1
            self.static.add(prim, nid)
        # pcb_PrimitivePad.getAll also lists component pads; only the bare M3 holes get a screw-head keep-out
        for h in snap["free"]:
            if h["d"] >= 3.0:
                self.static.add(Prim("cap", (h["x"], h["y"], h["x"], h["y"], 2.8), -1, (0, 1), "hole"), -1)
        for x, y, r in snap.get("posts", []):  # NPTH locating posts drawn as footprint fills
            self.static.add(Prim("cap", (x, y, x, y, r), -1, (0, 1), "post"), -1)
        for x1, y1, x2, y2, layers in parts.KEEPOUTS[:2]:
            lay = tuple(0 if l == "F.Cu" else 1 for l in layers)
            self.static.add(Prim("rect", ((x1 + x2) / 2, (y1 + y2) / 2, (x2 - x1) / 2, (y2 - y1) / 2), -1, lay, "keepout"), -1)
        self.fixed = {"tracks": [], "vias": []}
        for net, layer, x1, y1, x2, y2, w in snap["lines"]:
            t = (net, 0 if layer == 1 else 1, x1, y1, x2, y2, w)
            self.fixed["tracks"].append(t)
            nid = self.net_id.get(net, -1) if net else -1
            self.static.add(Prim("cap", (x1, y1, x2, y2, w / 2), net, (t[1],), "track"), nid)
        for net, x, y, d in snap["vias"]:
            self.fixed["vias"].append((net, x, y, d, d / 2))
            nid = self.net_id.get(net, -1) if net else -1
            self.static.add(Prim("cap", (x, y, x, y, d / 2), net, (0, 1), "via"), nid)
        self.routes = {}
        self.dm = None
        self.rebuild()

    def groups(self, net):
        # grow the tree from the fine-pitch receptacle first so long runs leave it at full width
        gs = super().groups(net)
        if net in parts.POWER_NETS:
            gs.sort(key=lambda g: not any(k in parts.FINE_PAD_W for k in g[0]))
        return gs

    def connect(self, net, tree, fine_cells, keys, cells, dm=None):
        """Like Router.connect, but steps the width down before falling back to the fine-pitch width."""
        full = router.net_width(net)
        if not any(k in parts.FINE_PAD_W for k in keys):
            coarse = tree - fine_cells
            for w in sorted({full, min(full, 0.4), min(full, 0.3)}, reverse=True):
                if coarse:
                    p = self.search(net, coarse, cells, w, dm)
                    if p is not None:
                        return p, w
        return super().connect(net, tree, fine_cells, keys, cells, dm)


def main():
    snap = json.load(open(sys.argv[1], encoding="utf-8"))
    if isinstance(snap, str):
        snap = json.loads(snap)
    out_path, nets = sys.argv[2], sys.argv[3:]
    # EasyEDA rule is 6 mil (0.152 mm); stay above it while fitting between 0.5 mm pitch pads
    parts.CLEARANCE = 0.18
    parts.FINE_PAD_W = {(p["ref"], p["num"]): 0.2 for p in snap["pads"] if p["ref"] in FINE_REFS and p["layer"] != 12}
    r = SnapRouter(snap)
    failed = {}
    for net in nets:
        f = r.route_net(net)
        if f:
            failed[net] = [k for k, _, _ in f]
    mil = lambda v: v / 0.0254
    out = {"tracks": [], "vias": [], "failed": failed}
    for net, rt in r.routes.items():
        for _, layer, x1, y1, x2, y2, w in rt["tracks"]:
            out["tracks"].append([net, 1 if layer == 0 else 2, mil(x1 + 100), -mil(y1 + 100), mil(x2 + 100), -mil(y2 + 100), mil(w)])
        for _, x, y, d, drill in rt["vias"]:
            out["vias"].append([net, mil(x + 100), -mil(y + 100), mil(d), mil(drill)])
    json.dump(out, open(out_path, "w"), indent=0)
    print("failed:", failed, "tracks:", len(out["tracks"]), "vias:", len(out["vias"]))


if __name__ == "__main__":
    main()
