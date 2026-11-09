"""Exact geometric DRC and connectivity check over pads, tracks and vias."""
import itertools

import parts
from geometry import Prim, board_pads, edge_dist, prim_dist


def copper(router):
    items = []
    for c, p, prim in board_pads():
        items.append(prim)
    for net, layer, x1, y1, x2, y2, w in router.tracks:
        items.append(Prim("cap", (x1, y1, x2, y2, w / 2), net, (layer,), ("track", net)))
    for net, x, y, d, drill in router.vias:
        items.append(Prim("cap", (x, y, x, y, d / 2), net, (0, 1), ("via", net)))
    return items


def run(router, clearance=parts.CLEARANCE):
    items = copper(router)
    errors = []
    # spatial hash
    cell = 2.0
    buckets = {}
    for idx, it in enumerate(items):
        x1, y1, x2, y2 = it.bbox()
        for gx in range(int(x1 // cell), int(x2 // cell) + 1):
            for gy in range(int(y1 // cell), int(y2 // cell) + 1):
                buckets.setdefault((gx, gy), []).append(idx)
    pairs = set()
    for lst in buckets.values():
        for a, b in itertools.combinations(lst, 2):
            pairs.add((a, b) if a < b else (b, a))
    parent = list(range(len(items)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b in pairs:
        p, q = items[a], items[b]
        if not set(p.layers) & set(q.layers):
            continue
        d = prim_dist(p, q)
        same = p.net is not None and p.net == q.net and p.net != -1
        if same:
            if d <= 1e-6:
                parent[find(a)] = find(b)
            continue
        if p.net == -1 or q.net == -1:
            need = clearance  # hole wall
        else:
            need = clearance
        if d < need - 1e-6:
            errors.append(f"clearance {d:.3f} < {need}: {p.owner}/{p.net} vs {q.owner}/{q.net}")
    for it in items:
        if it.net == -1:
            continue
        e = edge_dist(it)
        if e < parts.EDGE_CLEARANCE - 1e-6 and it.owner[0] not in ("J1",):
            errors.append(f"edge {e:.3f}: {it.owner}/{it.net}")
    for x1, y1, x2, y2, layers in parts.KEEPOUTS:
        ko = Prim("rect", ((x1 + x2) / 2, (y1 + y2) / 2, (x2 - x1) / 2, (y2 - y1) / 2), -1,
                  tuple(0 if l == "F.Cu" else 1 for l in layers), "keepout")
        for it in items:
            if isinstance(it.owner, tuple) and it.owner[0] in ("track", "via"):
                if set(it.layers) & set(ko.layers) and prim_dist(it, ko) < 1e-6:
                    errors.append(f"keepout hit {it.owner} at {it.a[:2]}")
    # connectivity: every net must form one island containing all its pads
    nets = {}
    for idx, it in enumerate(items):
        if isinstance(it.net, str):
            nets.setdefault(it.net, set()).add(find(idx))
    opens = {n: len(s) for n, s in nets.items() if len(s) > 1}
    return errors, opens
