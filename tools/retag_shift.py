"""Write logs/<out>.awt: a tile file shifted so that loading it at the hero's CURRENT position puts it
exactly where an earlier load's origin was (so a second copy, e.g. with another shape tag, overlaps
the first). python retag_shift.py <src.awt> <out> [origin x y z]   (origin defaults to the first load)"""
import os, struct, sys
import numpy as np
import gotham_overlay as g
from sm_proc import Proc

src, out = sys.argv[1], sys.argv[2]
origin = np.array([float(x) for x in sys.argv[3:6]]) if len(sys.argv) >= 6 else g.loads()[0][2]
hero = Proc().hero_matrix()[3, :3].astype(np.float64)
shift = origin - hero
with open(os.path.join(g.LOGS, src), "rb") as f, open(os.path.join(g.LOGS, out + ".awt"), "wb") as o:
	assert f.read(4) == b"AWT1"
	n = struct.unpack("<I", f.read(4))[0]
	o.write(b"AWT1"); o.write(struct.pack("<I", n))
	for _ in range(n):
		cx, cy, cz, nv, nt = struct.unpack("<3fII", f.read(20))
		c = np.array([cx, cy, cz]) + shift
		o.write(struct.pack("<3fII", *c.astype(np.float32), nv, nt))
		o.write(f.read(12 * nv)); o.write(f.read(6 * nt))
print("shift %s (hero %s -> origin %s)" % (np.round(shift, 2), np.round(hero, 2), np.round(origin, 2)))
