"""PNG preview of placement and routing (front view)."""
import matplotlib

matplotlib.use("Agg")
import matplotlib.patches as mp
import matplotlib.pyplot as plt

import parts
from geometry import board_pads

COL = {0: "#c83232", 1: "#3264c8"}


def render(r, path, title="wireTFT carrier (front view)", window=None):
    fig, ax = plt.subplots(figsize=(7, 10) if window is None else (10, 6), dpi=160)
    ax.add_patch(mp.Rectangle((0, 0), parts.BOARD_W, parts.BOARD_H, fc="#123a12", ec="k", lw=1))
    x1, y1, x2, y2 = parts.SCREEN
    ax.add_patch(mp.Rectangle((x1, y1), x2 - x1, y2 - y1, fc="none", ec="#cccccc", ls="--", lw=0.8))
    for kx1, ky1, kx2, ky2, layers in parts.KEEPOUTS:
        ax.add_patch(mp.Rectangle((kx1, ky1), kx2 - kx1, ky2 - ky1, fc="none", ec="#ffaa00", ls=":", lw=0.8))
    for net, layer, ax1, ay1, ax2, ay2, w in sorted(r.tracks, key=lambda t: -t[1]):
        yr = (parts.BOARD_H + 2) if window is None else (window[3] - window[1])
        xr = (parts.BOARD_W + 2) if window is None else (window[2] - window[0])
        fw, fh = fig.get_size_inches()
        ppm = min(fh * 72 * 0.92 / yr, fw * 72 * 0.92 / xr)
        ax.plot([ax1, ax2], [ay1, ay2], color=COL[layer], lw=w * ppm, solid_capstyle="round", alpha=0.75)
    for c, p, prim in board_pads():
        if prim.kind == "rect":
            cx, cy, hx, hy = prim.a
            ax.add_patch(mp.Rectangle((cx - hx, cy - hy), 2 * hx, 2 * hy, fc="#d4af37" if prim.net != -1 else "k", ec="none"))
        else:
            ax1_, ay1_, ax2_, ay2_, rr = prim.a
            fc = "k" if prim.net == -1 else "#d4af37"
            ax.add_patch(mp.FancyBboxPatch((min(ax1_, ax2_) - rr, min(ay1_, ay2_) - rr), abs(ax2_ - ax1_) + 2 * rr,
                                           abs(ay2_ - ay1_) + 2 * rr, boxstyle=f"round,pad=0,rounding_size={rr}", fc=fc, ec="none"))
    for net, x, y, d, drill in r.vias:
        ax.add_patch(mp.Circle((x, y), d / 2, fc="#bbbbbb", ec="none"))
        ax.add_patch(mp.Circle((x, y), drill / 2, fc="k", ec="none"))
    for c in parts.COMPONENTS:
        if c["ref"].startswith("H"):
            continue
        x, y = c["at"]
        ax.text(x, y, c["ref"], color="white", fontsize=5, ha="center", va="center")
    if window:
        ax.set_xlim(window[0], window[2])
        ax.set_ylim(window[3], window[1])
    else:
        ax.set_xlim(-1, parts.BOARD_W + 1)
        ax.set_ylim(parts.BOARD_H + 1, -1)
    ax.set_aspect("equal")
    ax.set_title(title, fontsize=9)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
