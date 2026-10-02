"""hole_check.py: every cached tile scan meshed like the streamer does; reports points (on a 2.5 m grid)
under which there is no floor at all - places Spider-Man would fall through.

  python hole_check.py [tile name ...]
"""
import glob
import os
import sys

import numpy as np

import pe_tools  # noqa: F401  (idle priority, one core)
import gotham_stream as gs


def floors(v, t, x, z):
	a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]

	def s(p, q):
		return (p[:, 0] - x) * (q[:, 2] - z) - (q[:, 0] - x) * (p[:, 2] - z)

	s1, s2, s3 = s(a, b), s(b, c), s(c, a)
	return np.count_nonzero(((s1 >= 0) & (s2 >= 0) & (s3 >= 0)) | ((s1 <= 0) & (s2 <= 0) & (s3 <= 0)))


def main():
	names = sys.argv[1:] or [os.path.basename(p)[:-5] for p in glob.glob(os.path.join(gs.STREAM, "t*.scan"))]
	worst = []
	for n in sorted(names):
		cols, h, grid = gs.read_scan(os.path.join(gs.STREAM, n + ".scan"))
		if not grid: continue
		v, t, w = gs.tile_scan_mesh(cols, h)
		x0, y0, nx, ny = grid
		holes = []
		for gx in np.arange(x0 + 125, x0 + 5000, 250):
			for gy in np.arange(y0 + 125, y0 + 5000, 250):
				if not floors(v, t, gx / 100, gy / 100): holes.append((round(gx), round(gy)))
		worst.append((len(holes), n, holes[:3]))
	worst.sort(reverse=True)
	print("%d tiles; points without any floor (of 400 per tile):" % len(worst))
	for cnt, n, ex in worst[:12]:
		print("  %-12s %3d  %s" % (n, cnt, ex))
	print("tiles with holes: %d" % sum(1 for w in worst if w[0]))


if __name__ == "__main__":
	main()
