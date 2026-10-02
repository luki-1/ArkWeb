"""Split a Gotham hull file into loading stages by bounding-box distance from its origin, so big
ground pieces come with the first stage.

  python gotham_stage.py near <radius>          -> logs/gotham_near.bin (bbox within radius)
  python gotham_stage.py far <radius> x y z     -> logs/gotham_far.bin (the rest), shifted so that
                                                    loading it at the hero's current position puts its
                                                    origin at x y z (where 'near' was loaded)
"""
import os
import struct
import sys

import numpy as np

import gotham_overlay as g
from sm_proc import Proc


def main():
	mode, radius = sys.argv[1], float(sys.argv[2])
	hulls = g.read_hulls(os.path.join(g.LOGS, "gotham_150m.bin"))
	lo = np.array([h.min(0) for h in hulls]); hi = np.array([h.max(0) for h in hulls])
	d = np.linalg.norm(np.maximum(np.maximum(lo, -hi), 0), axis=1)
	pick = np.nonzero(d < radius if mode == "near" else d >= radius)[0]
	shift = np.zeros(3)
	if mode == "far":
		hero = Proc().hero_matrix()[3, :3].astype(np.float64)
		shift = np.array([float(x) for x in sys.argv[3:6]]) - hero
	out = os.path.join(g.LOGS, "gotham_%s.bin" % mode)
	with open(out, "wb") as f:
		f.write(b"AWG1")
		f.write(struct.pack("<I", len(pick)))
		for i in pick:
			pts = (hulls[i] + shift).astype(np.float32)
			f.write(struct.pack("<I", len(pts)))
			f.write(pts.tobytes())
	print("%s: %d hulls -> %s (shift %s)" % (mode, len(pick), out, np.round(shift, 2)))


if __name__ == "__main__":
	main()
