"""Write logs/gotham_fill.bin: the hulls of a Gotham file that reach within R meters of its origin
(by bounding box, so big ground pieces count) but weren't in the first load (which picked the
nearest N by center), shifted so that loading the file at the hero's current position puts them
exactly where the first load's origin was. Run right before 'gotham load gotham_fill.bin' while
the hero stands still.

  python gotham_fill.py [radius m]     -> prints the command to send
"""
import os
import struct
import sys

import numpy as np

import gotham_overlay as g
from sm_proc import Proc


def main():
	radius = float(sys.argv[1]) if len(sys.argv) > 1 else 100
	loads = g.loads()
	fn, limit, origin = loads[0]
	hulls = g.read_hulls(os.path.join(g.LOGS, fn))
	order = np.argsort([float((h.mean(0) ** 2).sum()) for h in hulls], kind="stable")
	loaded = set(order[:limit].tolist()) if limit else set(range(len(hulls)))
	lo = np.array([h.min(0) for h in hulls]); hi = np.array([h.max(0) for h in hulls])
	aabb_d = np.linalg.norm(np.maximum(np.maximum(lo, -hi), 0), axis=1)
	fill = [i for i in np.nonzero(aabb_d < radius)[0] if i not in loaded]
	hero = Proc().hero_matrix()[3, :3].astype(np.float64)
	shift = origin - hero
	out = os.path.join(g.LOGS, "gotham_fill.bin")
	with open(out, "wb") as f:
		f.write(b"AWG1")
		f.write(struct.pack("<I", len(fill)))
		for i in fill:
			pts = (hulls[i] + shift).astype(np.float32)
			f.write(struct.pack("<I", len(pts)))
			f.write(pts.tobytes())
	print("%d fill hulls (bbox within %.0f m of %s), shifted by %s for hero at %s" % (len(fill), radius, np.round(origin, 2),
	      np.round(shift, 2), np.round(hero, 2)))


if __name__ == "__main__":
	main()
