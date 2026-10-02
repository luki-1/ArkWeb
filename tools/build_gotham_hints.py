"""Build Spider-Man swing hints for Gotham (logs/<name>.awh).

Spider-Man does not find web anchors in its collision: the SwingPointHunter queries a database of
oriented boxes (exe+5da0648, the game's swing "hint" volumes, 0x70-byte records: rotation rows,
position, half extents, ...) and puts candidate attach points on their faces, then checks them with
rays. Gotham has none, so the hunter finds zero candidates. This makes one box per Gotham building
part from Arkham's PhysX export (boxes exactly; tall convex hulls as an upright box around their
minimum-area footprint), in guest space relative to Batman's feet at scan time - the same origin as
build_gotham_tiles.py, so `gotham hints` must be loaded at the same origin as the tiles.

File "AWH1": u32 count; per hint 15 floats: row0 xyz, row1 xyz (up), row2 xyz, position xyz,
half extents (along row0, row1, row2).

  python build_gotham_hints.py [--radius 150] [--min-height 4] [--max 5000]
"""
import argparse
import os
import struct

import numpy as np
from scipy.spatial import ConvexHull

import pe_tools  # noqa: F401  (idle priority)
from build_gotham_tiles import HALF_HEIGHT_UU, LOGS, header_batman, to_guest


def box_obb(c):
	"""8 box corners (guest) -> (rows 3x3, center, half extents) with row1 the most vertical axis."""
	d = np.linalg.norm(c[1:] - c[0], axis=1)
	axes = c[1:][np.argsort(d)[:3]] - c[0]
	lengths = np.linalg.norm(axes, axis=1)
	if np.any(lengths < 1e-3): return None
	axes = axes / lengths[:, None]
	up = int(np.argmax(np.abs(axes[:, 1])))
	order = [i for i in range(3) if i != up]
	r1 = axes[up] * np.sign(axes[up][1] or 1)
	r0 = axes[order[0]]
	r2 = np.cross(r0, r1)
	half = np.array([lengths[order[0]], lengths[up], lengths[order[1]]]) / 2
	return np.array([r0, r1, r2]), c.mean(0), half


def hull_obb(p):
	"""Upright box around a point set: minimum-area rectangle of the xz footprint, full y range."""
	xz = p[:, [0, 2]]
	try:
		h = ConvexHull(xz)
	except Exception:
		return None
	pts = xz[h.vertices]
	best = None
	for i in range(len(pts)):
		e = pts[(i + 1) % len(pts)] - pts[i]
		n = np.linalg.norm(e)
		if n < 1e-6: continue
		u = e / n
		v = np.array([-u[1], u[0]])
		a, b = pts @ u, pts @ v
		area = np.ptp(a) * np.ptp(b)
		if best is None or area < best[0]:
			best = (area, u, v, (a.min() + a.max()) / 2, (b.min() + b.max()) / 2, np.ptp(a) / 2, np.ptp(b) / 2)
	if best is None: return None
	_, u, v, ca, cb, ha, hb = best
	r0 = np.array([u[0], 0.0, u[1]])
	r1 = np.array([0.0, 1.0, 0.0])
	r2 = np.cross(r0, r1)
	cxz = ca * u + cb * v
	y0, y1 = p[:, 1].min(), p[:, 1].max()
	# r2 = r0 x r1 may point along -v; the half extent along it is hb either way
	return np.array([r0, r1, r2]), np.array([cxz[0], (y0 + y1) / 2, cxz[1]]), np.array([ha, (y1 - y0) / 2, hb])


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--radius", type=float, default=150)
	ap.add_argument("--min-height", type=float, default=4)
	ap.add_argument("--min-width", type=float, default=2)
	ap.add_argument("--max", type=int, default=5000)
	ap.add_argument("--min-top", type=float, default=-20, help="drop boxes whose top is lower (m, relative to Batman's feet)")
	ap.add_argument("--scan", default="gotham_scan.scan")
	ap.add_argument("--physx", default="gotham_here.obj")
	ap.add_argument("--out", default="gotham_hints")
	a = ap.parse_args()
	feet = header_batman(os.path.join(LOGS, a.scan)) - np.array([0, 0, HALF_HEIGHT_UU])
	verts, groups, cur = [], [], None
	with open(os.path.join(LOGS, a.physx)) as f:
		for line in f:
			if line.startswith("o "):
				cur = {"v0": len(verts), "faces": 0}
				groups.append(cur)
			elif line.startswith("v "):
				verts.append([float(x.replace("(ind)", "")) for x in line.split()[1:4]])  # MSVC writes NaN as -nan(ind)
			elif line.startswith("f "):
				cur["faces"] += 1
	verts = to_guest(np.array(verts), feet)
	hints, boxes, hulls = [], 0, 0
	for gi, g in enumerate(groups):
		v1 = groups[gi + 1]["v0"] if gi + 1 < len(groups) else len(verts)
		p = verts[g["v0"]:v1]
		if len(p) < 4 or not np.all(np.isfinite(p)) or np.hypot(*p.mean(0)[[0, 2]]) > a.radius: continue
		if np.ptp(p[:, 1]) < a.min_height: continue
		obb = box_obb(p) if len(p) == 8 and g["faces"] == 12 else hull_obb(p)
		if obb is None: continue
		rows, center, half = obb
		if 2 * min(half[0], half[2]) < a.min_width: continue
		if center[1] + half[1] < a.min_top: continue  # foundations and tunnels below the streets
		hints.append((rows, center, half))
		if len(p) == 8: boxes += 1
		else: hulls += 1
	# biggest first, then cap
	hints.sort(key=lambda h: -np.prod(h[2]))
	hints = hints[:a.max]
	out = os.path.join(LOGS, a.out + ".awh")
	with open(out, "wb") as f:
		f.write(b"AWH1")
		f.write(struct.pack("<I", len(hints)))
		for rows, center, half in hints:
			f.write(np.concatenate([rows.ravel(), center, half]).astype(np.float32).tobytes())
	tops = np.array([c[1] + h[1] for _, c, h in hints])
	print("%d hints (%d boxes, %d hulls) -> %s; tops %.0f..%.0f m above Batman's feet" % (len(hints), boxes, hulls, out, tops.min(), tops.max()))


if __name__ == "__main__":
	main()
