"""Two-layer grid A* router (C core) with exact-distance clearance maps and rip-up/reroute."""
import ctypes
import math
import os

import numpy as np

import parts
from geometry import Prim, board_pads, pt_seg

G = 0.1
NX = int(round(parts.BOARD_W / G)) + 1
NY = int(round(parts.BOARD_H / G)) + 1
CELLS = NX * NY
MARGIN = 0.015
REACH = 1.0
VIA_COST = 30.0
LAYER_COST = (1.0, 1.25)
TURN_COST = (0.0, 0.4, 1.6, 6.0, 50.0)

XS = np.arange(NX) * G
YS = np.arange(NY) * G

_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "astar.dll"))
_u8 = ctypes.POINTER(ctypes.c_uint8)
_i32 = ctypes.POINTER(ctypes.c_int32)
_f32 = ctypes.POINTER(ctypes.c_float)
_lib.astar.restype = ctypes.c_int
_lib.astar.argtypes = [ctypes.c_int, ctypes.c_int, _u8, _u8, _u8, ctypes.c_int, _i32,
                       ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       ctypes.c_float, _f32, _f32, ctypes.c_int, _i32, ctypes.c_int]
_layer_cost = (ctypes.c_float * 2)(*LAYER_COST)
_turn_cost = (ctypes.c_float * 5)(*TURN_COST)
_out = (ctypes.c_int32 * 200000)()


def net_width(net):
    return parts.NET_W.get(net, parts.SIGNAL_W)


def via_size(net):
    if net in parts.POWER_NETS:
        return parts.POWER_VIA_D, parts.POWER_VIA_DRILL
    return parts.VIA_D, parts.VIA_DRILL


class DistMap:
    """Per layer: nearest copper distance + its net id, and nearest distance of any other net."""

    def __init__(self, base=None):
        if base is None:
            self.d1 = np.full((2, NY, NX), np.inf, dtype=np.float32)
            self.n1 = np.zeros((2, NY, NX), dtype=np.int32)
            self.d2 = np.full((2, NY, NX), np.inf, dtype=np.float32)
        else:
            self.d1, self.n1, self.d2 = base.d1.copy(), base.n1.copy(), base.d2.copy()

    def add(self, prim, net_id):
        x1, y1, x2, y2 = prim.bbox()
        i0 = max(0, int(math.floor((x1 - REACH) / G)))
        i1 = min(NX - 1, int(math.ceil((x2 + REACH) / G)))
        j0 = max(0, int(math.floor((y1 - REACH) / G)))
        j1 = min(NY - 1, int(math.ceil((y2 + REACH) / G)))
        X = XS[i0:i1 + 1][None, :]
        Y = YS[j0:j1 + 1][:, None]
        if prim.kind == "rect":
            cx, cy, hx, hy = prim.a
            d = np.hypot(np.maximum(np.abs(X - cx) - hx, 0), np.maximum(np.abs(Y - cy) - hy, 0))
        else:
            ax, ay, bx, by, r = prim.a
            dx, dy = bx - ax, by - ay
            L = dx * dx + dy * dy
            t = np.zeros((Y.shape[0], X.shape[1])) if L == 0 else np.clip(((X - ax) * dx + (Y - ay) * dy) / L, 0, 1)
            d = np.maximum(np.hypot(X - ax - t * dx, Y - ay - t * dy) - r, 0)
        d = d.astype(np.float32)
        for L_ in prim.layers:
            D1 = self.d1[L_, j0:j1 + 1, i0:i1 + 1]
            N1 = self.n1[L_, j0:j1 + 1, i0:i1 + 1]
            D2 = self.d2[L_, j0:j1 + 1, i0:i1 + 1]
            same = N1 == net_id
            np.minimum(D1, np.where(same, d, D1), out=D1)
            other = ~same
            closer = other & (d < D1)
            D2[closer] = D1[closer]
            D1[closer] = d[closer]
            N1[closer] = net_id
            farther = other & ~closer
            np.minimum(D2, np.where(farther, d, D2), out=D2)

    def other(self, net_id):
        return np.where(self.n1 == net_id, self.d2, self.d1)


def edge_map():
    X = XS[None, :]
    Y = YS[:, None]
    return np.minimum(np.minimum(X, parts.BOARD_W - X), np.minimum(Y, parts.BOARD_H - Y)).astype(np.float32)


def cells_in(prim):
    x1, y1, x2, y2 = prim.bbox()
    out = []
    for j in range(max(0, int(math.ceil(y1 / G - 1e-9))), min(NY - 1, int(math.floor(y2 / G + 1e-9))) + 1):
        for i in range(max(0, int(math.ceil(x1 / G - 1e-9))), min(NX - 1, int(math.floor(x2 / G + 1e-9))) + 1):
            x, y = i * G, j * G
            if prim.kind == "rect":
                cx, cy, hx, hy = prim.a
                inside = abs(x - cx) <= hx + 1e-9 and abs(y - cy) <= hy + 1e-9
            else:
                ax, ay, bx, by, r = prim.a
                inside = pt_seg(x, y, ax, ay, bx, by) <= r + 1e-9
            if inside:
                out.append((i, j))
    return out


def track_cells(layer, x1, y1, x2, y2, w):
    prim = Prim("cap", (x1, y1, x2, y2, w / 2), None, (layer,), None)
    return {(layer, i, j) for i, j in cells_in(prim)}


class Router:
    def __init__(self):
        self.pads = board_pads()
        self.nets = sorted({p.net for _, _, p in self.pads if isinstance(p.net, str)})
        self.net_id = {n: i + 1 for i, n in enumerate(self.nets)}
        self.id_net = {i: n for n, i in self.net_id.items()}
        self.edge = edge_map()
        self.static = DistMap()
        uid = -2
        for c, p, prim in self.pads:
            if prim.net == -1:
                nid = -1
            elif prim.net:
                nid = self.net_id[prim.net]
            else:
                nid = uid
                uid -= 1
            self.static.add(prim, nid)
        for x1, y1, x2, y2, layers in parts.KEEPOUTS:
            lay = tuple(0 if l == "F.Cu" else 1 for l in layers)
            self.static.add(Prim("rect", ((x1 + x2) / 2, (y1 + y2) / 2, (x2 - x1) / 2, (y2 - y1) / 2), -1, lay, "keepout"), -1)
        for hx, hy in parts.MOUNT_HOLES:
            self.static.add(Prim("cap", (hx, hy, hx, hy, 2.8), -1, (0, 1), "hole"), -1)
        self.fixed = {"tracks": [], "vias": []}
        self.routes = {}  # net -> {"tracks": [...], "vias": [...]}
        self._prefix()
        self.dm = None
        self.rebuild()

    # ---------------------------------------------------------------- copper bookkeeping
    def _prefix(self):
        for net, layer, pts, w in parts.FIXED_TRACKS:
            for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
                self.fixed["tracks"].append((net, layer, x1, y1, x2, y2, w))
        for net, x, y in parts.FIXED_VIAS:
            d, drill = via_size(net)
            self.fixed["vias"].append((net, x, y, d, drill))
        base = self.static
        for t in self.fixed["tracks"]:
            base.add(Prim("cap", (t[2], t[3], t[4], t[5], t[6] / 2), t[0], (t[1],), "track"), self.net_id[t[0]])
        for v in self.fixed["vias"]:
            base.add(Prim("cap", (v[1], v[2], v[1], v[2], v[3] / 2), v[0], (0, 1), "via"), self.net_id[v[0]])

    def rebuild(self):
        self.dm = DistMap(self.static)
        for net, rt in self.routes.items():
            nid = self.net_id[net]
            for t in rt["tracks"]:
                self.dm.add(Prim("cap", (t[2], t[3], t[4], t[5], t[6] / 2), net, (t[1],), "track"), nid)
            for v in rt["vias"]:
                self.dm.add(Prim("cap", (v[1], v[2], v[1], v[2], v[3] / 2), net, (0, 1), "via"), nid)

    @property
    def tracks(self):
        out = list(self.fixed["tracks"])
        for rt in self.routes.values():
            out += rt["tracks"]
        return out

    @property
    def vias(self):
        out = list(self.fixed["vias"])
        for rt in self.routes.values():
            out += rt["vias"]
        return out

    # ---------------------------------------------------------------- search
    def _maps(self, dm, net, width, extra_free):
        nid = self.net_id[net]
        other = dm.other(nid)
        need = width / 2 + parts.CLEARANCE + MARGIN
        free = (other >= need) & (self.edge[None] >= width / 2 + parts.EDGE_CLEARANCE)
        vd, _ = via_size(net)
        vneed = vd / 2 + parts.CLEARANCE + MARGIN
        via_ok = (other[0] >= vneed) & (other[1] >= vneed) & (self.edge >= vd / 2 + parts.EDGE_CLEARANCE)
        for L, i, j in extra_free:
            free[L, j, i] = True
        return np.ascontiguousarray(free, dtype=np.uint8), np.ascontiguousarray(via_ok, dtype=np.uint8)

    def search(self, net, src_cells, dst_cells, width, dm=None, max_exp=3_000_000):
        dm = dm or self.dm
        free, via_ok = self._maps(dm, net, width, list(src_cells) + list(dst_cells))
        dst = np.zeros(2 * CELLS, dtype=np.uint8)
        for L, i, j in dst_cells:
            dst[L * CELLS + j * NX + i] = 1
        di = [c[1] for c in dst_cells]
        dj = [c[2] for c in dst_cells]
        cx, cy = sum(di) / len(di), sum(dj) / len(dj)
        near = min(src_cells, key=lambda c: abs(c[1] - cx) + abs(c[2] - cy))
        fp, vp, dp = (a.ctypes.data_as(_u8) for a in (free, via_ok, dst))
        for expand in (40, 120, 100000):
            win = (max(0, min(min(di), near[1]) - expand), max(0, min(min(dj), near[2]) - expand),
                   min(NX - 1, max(max(di), near[1]) + expand), min(NY - 1, max(max(dj), near[2]) + expand))
            src = [c for c in src_cells if win[0] <= c[1] <= win[2] and win[1] <= c[2] <= win[3]]
            if not src:
                continue
            arr = (ctypes.c_int32 * (2 * len(src)))(*[v for L, i, j in src for v in (L, j * NX + i)])
            n = _lib.astar(NX, NY, fp, vp, dp, len(src), arr, win[0], win[1], win[2], win[3],
                           min(di), min(dj), max(di), max(dj), VIA_COST, _layer_cost, _turn_cost,
                           max_exp, _out, len(_out))
            if n > 0:
                path = []
                for k in range(n - 1, -1, -1):
                    li = _out[k]
                    L, idx = divmod(li, CELLS)
                    path.append((L, idx % NX, idx // NX))
                return path
            if win == (0, 0, NX - 1, NY - 1):
                break
        return None

    # ---------------------------------------------------------------- commit
    def path_to_copper(self, net, path, width):
        tracks, vias = [], []
        d, drill = via_size(net)
        segs = []
        start = prev = path[0]
        pdir = None
        for cur in path[1:]:
            if cur[0] != prev[0]:
                if prev != start:
                    segs.append((start, prev))
                vias.append((net, round(prev[1] * G, 4), round(prev[2] * G, 4), d, drill))
                start = prev = cur
                pdir = None
                continue
            dcur = (cur[1] - prev[1], cur[2] - prev[2])
            if pdir is not None and dcur != pdir:
                segs.append((start, prev))
                start = prev
            pdir = dcur
            prev = cur
        if prev != start:
            segs.append((start, prev))
        for a, b in segs:
            tracks.append((net, a[0], round(a[1] * G, 4), round(a[2] * G, 4), round(b[1] * G, 4), round(b[2] * G, 4), width))
        return tracks, vias

    def add_copper(self, net, tracks, vias):
        rt = self.routes.setdefault(net, {"tracks": [], "vias": []})
        rt["tracks"] += tracks
        rt["vias"] += vias
        nid = self.net_id[net]
        for t in tracks:
            self.dm.add(Prim("cap", (t[2], t[3], t[4], t[5], t[6] / 2), net, (t[1],), "track"), nid)
        for v in vias:
            self.dm.add(Prim("cap", (v[1], v[2], v[1], v[2], v[3] / 2), net, (0, 1), "via"), nid)

    # ---------------------------------------------------------------- nets
    def groups(self, net):
        groups = []
        for c, p, prim in self.pads:
            if prim.net == net:
                cells = {(L, i, j) for L in prim.layers for i, j in cells_in(prim)}
                groups.append(([(c["ref"], p["num"])], cells))
        fixed = [t for t in self.fixed["tracks"] if t[0] == net]
        for t in fixed:
            tc = track_cells(t[1], t[2], t[3], t[4], t[5], t[6])
            touching = [g for g in groups if g[1] & tc]
            if not touching:
                groups.append(([("fixed", net)], tc))
                continue
            base = touching[0]
            base[1].update(tc)
            for g in touching[1:]:
                base[0].extend(g[0])
                base[1].update(g[1])
                groups.remove(g)
        for v in [v for v in self.fixed["vias"] if v[0] == net]:
            i, j = int(round(v[1] / G)), int(round(v[2] / G))
            for g in groups:
                if (0, i, j) in g[1] or (1, i, j) in g[1]:
                    g[1].update({(0, i, j), (1, i, j)})
        # merge groups that now overlap (via joins both layers)
        merged = True
        while merged:
            merged = False
            for a in range(len(groups)):
                for b in range(a + 1, len(groups)):
                    if groups[a][1] & groups[b][1]:
                        groups[a][0].extend(groups[b][0])
                        groups[a][1].update(groups[b][1])
                        groups.pop(b)
                        merged = True
                        break
                if merged:
                    break
        fine = lambda g: any(k in parts.FINE_PAD_W for k in g[0])
        groups.sort(key=lambda g: (fine(g), -len(g[1])))
        return groups

    def route_net(self, net):
        """Returns list of failed (keys, cells, tree) tuples; committed copper goes to self.routes."""
        groups = self.groups(net)
        tree_keys, tree = groups[0][0][:], set(groups[0][1])
        fine_cells = set()
        for keys, cells in groups:
            if any(k in parts.FINE_PAD_W for k in keys):
                fine_cells |= cells
        remaining = groups[1:]
        failed = []
        progress = True
        while remaining and progress:
            progress = False
            pending, remaining = remaining, []
            while pending:
                tl = list(tree)[:: max(1, len(tree) // 300)]
                pending.sort(key=lambda g: min(abs(a[1] - b[1]) + abs(a[2] - b[2]) for a in list(g[1])[:30] for b in tl))
                keys, cells = pending.pop(0)
                path, width = self.connect(net, tree, fine_cells, keys, cells)
                if path is None:
                    remaining.append((keys, cells))
                    continue
                t, v = self.path_to_copper(net, path, width)
                self.add_copper(net, t, v)
                tree |= cells | set(path)
                for tt in t:
                    tree |= track_cells(*tt[1:])
                tree_keys += keys
                progress = True
        for keys, cells in remaining:
            failed.append((keys, cells, tree))
        return failed

    def connect(self, net, tree, fine_cells, keys, cells, dm=None):
        full = net_width(net)
        dst_fine = any(k in parts.FINE_PAD_W for k in keys)
        cap = min([full] + [parts.FINE_PAD_W[k] for k in keys if k in parts.FINE_PAD_W])
        coarse_src = tree - fine_cells
        if coarse_src and not dst_fine:
            p = self.search(net, coarse_src, cells, full, dm)
            if p is not None:
                return p, full
        fine_cap = min(cap, min(parts.FINE_PAD_W.values()))
        p = self.search(net, tree, cells, fine_cap if (tree & fine_cells or dst_fine) else full, dm)
        return p, (fine_cap if (tree & fine_cells or dst_fine) else full)

    def rip(self, net):
        self.routes.pop(net, None)

    def blockers(self, net, failure):
        """Nets whose routed copper sits on the path this connection would take if they were absent."""
        keys, cells, tree = failure
        soft = DistMap(self.static)
        fine_cells = set()
        for ks, cs in self.groups(net):
            if any(k in parts.FINE_PAD_W for k in ks):
                fine_cells |= cs
        own = self.routes.get(net, {"tracks": [], "vias": []})
        nid = self.net_id[net]
        for t in own["tracks"]:
            soft.add(Prim("cap", (t[2], t[3], t[4], t[5], t[6] / 2), net, (t[1],), "track"), nid)
        path, width = self.connect(net, tree, fine_cells, keys, cells, dm=soft)
        if path is None:
            return None
        need = width / 2 + parts.CLEARANCE + MARGIN
        other = self.dm.other(nid)
        found = set()
        vd, _ = via_size(net)
        for k, (L, i, j) in enumerate(path):
            layers = (L,)
            r = need
            if k + 1 < len(path) and path[k + 1][0] != L:
                layers, r = (0, 1), vd / 2 + parts.CLEARANCE + MARGIN
            for LL in layers:
                if other[LL, j, i] < r:
                    n = self.dm.n1[LL, j, i]
                    name = self.id_net.get(int(n))
                    if name and name != net and name in self.routes:
                        found.add(name)
        return found


def default_order(r):
    def hpwl(net):
        pts = [prim.a[:2] for _, _, prim in r.pads if prim.net == net]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return (max(xs) - min(xs)) + (max(ys) - min(ys))

    tail = ["+3V3", "GND"]
    nets = [n for n in r.nets if n not in tail]
    return sorted(nets, key=hpwl) + tail


def route_all(log=None, max_rips=60):
    log = log if log is not None else []
    r = Router()
    queue = default_order(r)
    rips = {}
    total = 0
    bad = set()
    while queue:
        net = queue.pop(0)
        r.rip(net)
        r.rebuild()
        failed = r.route_net(net)
        if not failed:
            bad.discard(net)
            continue
        bad.add(net)
        culprits = set()
        for f in failed:
            b = r.blockers(net, f)
            if b:
                culprits |= b
        culprits = {c for c in culprits if rips.get(c, 0) < 4}
        if not culprits or total >= max_rips:
            log.append(f"FAIL {net}: {[f[0] for f in failed]} (no rip-up candidates)")
            continue
        total += 1
        log.append(f"rip {sorted(culprits)} for {net}")
        for c in culprits:
            rips[c] = rips.get(c, 0) + 1
            r.rip(c)
        r.rip(net)
        r.rebuild()
        failed = r.route_net(net)
        if not failed:
            bad.discard(net)
        else:
            log.append(f"  {net} still failing after rip-up")
        queue = [c for c in culprits if c not in queue] + queue
        if failed:
            rips[net] = rips.get(net, 0) + 1
            if rips[net] <= 4:
                queue.append(net)
            else:
                log.append(f"FAIL {net}: gave up")
    return r, sorted(bad)
