"""Route the board, verify it, and write the KiCad project + BOM.

Usage: python build.py   (needs numpy, matplotlib; astar.dll is built from astar.c with gcc)
"""
import os
import shutil
import sys

import drc
import preview
import router
import write_kicad

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "kicad"))


def main():
    log = []
    r, failed = router.route_all(log=log)
    print("\n".join(log))
    errors, opens = drc.run(r)
    print(f"tracks={len(r.tracks)} vias={len(r.vias)} failed={failed} drc_errors={len(errors)} opens={opens}")
    for e in errors:
        print("  ", e)
    if failed or errors or opens:
        sys.exit(1)
    write_kicad.write_all(OUT, r)
    write_kicad.write_bom(os.path.join(OUT, "..", "BOM_JLCPCB.csv"))
    preview.render(r, os.path.join(OUT, "..", "preview_front.png"))
    preview.render(r, os.path.join(OUT, "..", "preview_bottom_strip.png"), "USB-C / CH340C / FPC area", window=(0, 61, 56, 84))
    archive = os.path.join(OUT, "..", write_kicad.PROJECT + "_kicad")
    shutil.make_archive(archive, "zip", os.path.dirname(OUT), os.path.basename(OUT))
    print("written to", OUT)


if __name__ == "__main__":
    main()
