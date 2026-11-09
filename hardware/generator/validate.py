"""Parse the generated KiCad files with kiutils and cross-check schematic connectivity against PCB pad nets."""
import glob
import os
import sys

from kiutils.board import Board
from kiutils.footprint import Footprint
from kiutils.schematic import Schematic
from kiutils.symbol import SymbolLib

HERE = os.path.dirname(os.path.abspath(__file__))
K = os.path.normpath(os.path.join(HERE, "..", "kicad"))


def key(x, y):
    return (round(x, 3), round(y, 3))


def schematic_nets(sch):
    libs = {s.libId if s.libId else s.entryName: s for s in sch.libSymbols}
    pin_pts = {}
    for inst in sch.schematicSymbols:
        lib = libs[inst.libId]
        X, Y = inst.position.X, inst.position.Y
        ref = next(p.value for p in inst.properties if p.key == "Reference")
        for unit in lib.units:
            for pin in unit.pins:
                pin_pts[(ref, pin.number)] = key(X + pin.position.X, Y - pin.position.Y)
    parent = {}

    def find(a):
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for w in sch.graphicalItems:
        if getattr(w, "type", None) == "wire":
            a, b = key(w.points[0].X, w.points[0].Y), key(w.points[1].X, w.points[1].Y)
            parent[find(a)] = find(b)
    label_at = {}
    for lb in sch.labels:
        label_at.setdefault(find(key(lb.position.X, lb.position.Y)), set()).add(lb.text)
    nc = {key(n.position.X, n.position.Y) for n in sch.noConnects}
    nets = {}
    for (ref, num), pt in pin_pts.items():
        names = label_at.get(find(pt), set())
        if len(names) > 1:
            print("label conflict", ref, num, names)
        if names:
            nets[(ref, num)] = next(iter(names))
        elif pt not in nc:
            print("floating pin without no-connect:", ref, num)
    return nets


def main():
    ok = True
    board = Board.from_file(os.path.join(K, "wireTFT_board.kicad_pcb"))
    sch = Schematic.from_file(os.path.join(K, "wireTFT_board.kicad_sch"))
    SymbolLib.from_file(os.path.join(K, "wiretft.kicad_sym"))
    for p in glob.glob(os.path.join(K, "wiretft.pretty", "*.kicad_mod")):
        Footprint.from_file(p)
    pcb = {}
    for fp in board.footprints:
        ref = next(t.text for t in fp.graphicItems if getattr(t, "type", None) == "reference")
        for pad in fp.pads:
            if pad.net and pad.net.name:
                prev = pcb.get((ref, pad.number))
                if prev and prev != pad.net.name:
                    print("pad net conflict", ref, pad.number, prev, pad.net.name)
                    ok = False
                pcb[(ref, pad.number)] = pad.net.name
    snets = schematic_nets(sch)
    for k in sorted(set(pcb) | set(snets)):
        if pcb.get(k) != snets.get(k):
            print("MISMATCH", k, "pcb", pcb.get(k), "sch", snets.get(k))
            ok = False
    print(f"footprints={len(board.footprints)} segments={sum(1 for i in board.traceItems if type(i).__name__ == 'Segment')} "
          f"vias={sum(1 for i in board.traceItems if type(i).__name__ == 'Via')} zones={len(board.zones)} "
          f"symbols={len(sch.schematicSymbols)} pins_with_nets={len(snets)} pcb_pads_with_nets={len(pcb)}")
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
