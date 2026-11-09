import sys
import time

import drc
import parts
import router

t = time.time()
log = []
r, failed = router.route_all(log=log)
print("\n".join(log))
print("failed nets:", failed, f"{time.time() - t:.1f}s")
errors, opens = drc.run(r)
print("DRC errors:", len(errors))
for e in errors[:40]:
    print("  ", e)
print("opens:", opens)
print("tracks", len(r.tracks), "vias", len(r.vias))
if "--plot" in sys.argv:
    import preview
    preview.render(r, "route_preview.png")
    preview.render(r, "route_zoom.png", "bottom strip", window=(0, 61, 56, 84))
