"""Build Gotham collision tiles for the guest (logs/<name>.awt) from Arkham's two sources:

  * the Unreal collision scan (logs/gotham_scan.scan, from the host's `scan` command): every
    surface a downward trace met, on a 2 m grid. Neighbouring columns whose surfaces are within
    1.5 m become floor triangles; where the highest surfaces of two neighbours differ by more than
    1 m a vertical wall is added between them (building sides, for wall runs and web anchors).
  * the PhysX static shapes (logs/gotham_here.obj, from `px export`): boxes and convex hulls,
    triangulated.

Everything goes to guest space (meters, Y up, right-handed: guest = (x, z, y) / 100) relative to
Batman's feet at scan time, and is cut into square tiles, each one hknpCompressedMeshShape on the
guest side (u16 indices, so at most 65535 vertices per tile; bigger tiles are split).

File "AWT1": u32 tileCount; per tile: f32 center[3], u32 nv, u32 nt, nv * f32[3] (relative to the
center), nt * u16[3].

  python build_gotham_tiles.py [--tile 50] [--radius 150] [--out gotham_tiles]
"""
import argparse
import os
import re
import struct

import numpy as np
from scipy.spatial import ConvexHull

import pe_tools  # noqa: F401  (idle priority, one core)

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
HALF_HEIGHT_UU = 95.0


def header_batman(path):
	with open(path) as f:
		for line in f:
			m = re.search(r"Batman ([-\d.]+) ([-\d.]+) ([-\d.]+)", line)
			if m: return np.array([float(x) for x in m.groups()])
	raise SystemExit("no Batman position in " + path)


def to_guest(p_uu, feet):
	r = (np.asarray(p_uu, np.float64) - feet) / 100.0
	return np.stack([r[..., 0], r[..., 2], r[..., 1]], axis=-1)


def scan_mesh(path, feet, quiet=False):
	"""Floor triangles + walls from the scan. Returns (verts guest Nx3, tris Mx3)."""
	hdr = open(path).readline()
	step = float(re.search(r"step ([\d.]+)", hdr).group(1))
	d = np.loadtxt(path, comments="#", ndmin=2)
	if d.size == 0:
		return np.zeros((0, 3)), np.zeros((0, 3), np.int64)
	cols = {}
	for i, j, x, y, z in d[:, :5]:
		cols.setdefault((int(i), int(j)), []).append((x, y, z))
	for k in cols: cols[k].sort(key=lambda s: -s[2])  # highest first
	verts, index = [], {}

	def vid(key, s):
		k = (key, round(s[2], 1))
		if k not in index:
			index[k] = len(verts)
			verts.append(s)
		return index[k]

	def match(key, z, tol=150.0):
		best = None
		for s in cols.get(key, ()):
			if abs(s[2] - z) <= tol and (best is None or abs(s[2] - z) < abs(best[2] - z)): best = s
		return best

	tris = []
	for (i, j), surfaces in cols.items():
		for s in surfaces:
			a = (i, j)
			b, c, e = (i + 1, j), (i, j + 1), (i + 1, j + 1)
			sb, sc, se = match(b, s[2]), match(c, s[2]), match(e, s[2])
			va = vid(a, s)
			if sb and se: tris.append((va, vid(b, sb), vid(e, se)))
			if se and sc: tris.append((va, vid(e, se), vid(c, sc)))
			if not se and sb and sc: tris.append((va, vid(b, sb), vid(c, sc)))
	# walls between the highest surfaces of 4-neighbours
	walls = 0
	for (i, j), surfaces in cols.items():
		top = surfaces[0]
		for nb, axis in (((i + 1, j), 0), ((i, j + 1), 1)):
			if nb not in cols: continue
			ntop = cols[nb][0]
			lo, hi = sorted((top[2], ntop[2]))
			if hi - lo < 100.0: continue
			# Only a building side, never an overpass or overhang: if the taller column also has a
			# surface at the lower column's height, there is open space underneath (a street under a
			# rail line, a ledge over a doorway) and a wall would block it.
			tall = cols[(i, j)] if top[2] > ntop[2] else cols[nb]
			if any(abs(s[2] - lo) < 150.0 for s in tall[1:]): continue
			mid = np.array([(top[0] + ntop[0]) / 2, (top[1] + ntop[1]) / 2])
			half = np.array([0.0, step / 2]) if axis == 0 else np.array([step / 2, 0.0])
			p0, p1 = mid - half, mid + half
			quad = [(p0[0], p0[1], lo), (p1[0], p1[1], lo), (p1[0], p1[1], hi), (p0[0], p0[1], hi)]
			base = len(verts)
			verts.extend(quad)
			tris.append((base, base + 1, base + 2))
			tris.append((base, base + 2, base + 3))
			walls += 1
	v = to_guest(np.array(verts), feet)
	if not quiet: print("scan: %d columns, %d vertices, %d triangles (%d walls)" % (len(cols), len(v), len(tris), walls))
	return v, np.array(tris, np.int64).reshape(-1, 3)


def physx_mesh(path, feet, radius_m):
	verts, groups, cur = [], [], None
	with open(path) as f:
		for line in f:
			if line.startswith("o "):
				cur = {"v0": len(verts), "f": []}
				groups.append(cur)
			elif line.startswith("v "):
				verts.append([float(x.replace("(ind)", "")) for x in line.split()[1:4]])  # MSVC writes NaN as -nan(ind)
			elif line.startswith("f "):
				cur["f"].append([int(x) - 1 for x in line.split()[1:4]])
	verts = to_guest(np.array(verts), feet)
	out_v, out_t, skipped = [], [], 0
	for gi, g in enumerate(groups):
		v1 = groups[gi + 1]["v0"] if gi + 1 < len(groups) else len(verts)
		pts = verts[g["v0"]:v1]
		if len(pts) == 0 or not np.all(np.isfinite(pts)) or np.hypot(*pts.mean(0)[[0, 2]]) > radius_m: continue
		base = sum(len(x) for x in out_v)
		if g["f"]:
			out_v.append(pts)
			out_t.append(np.array(g["f"]) - g["v0"] + base)
			continue
		try:
			h = ConvexHull(pts)
		except Exception:
			skipped += 1
			continue
		out_v.append(pts)
		out_t.append(h.simplices + base)
	v = np.concatenate(out_v)
	t = np.concatenate(out_t)
	print("physx: %d shapes, %d vertices, %d triangles (%d degenerate hulls skipped)" % (len(out_v), len(v), len(t), skipped))
	return v, t


def tiles(v, t, size, max_verts=65535):
	"""Assign each triangle to the tile of its centroid (xz); split tiles that are too big."""
	cent = v[t].mean(1)
	key = np.floor(cent[:, [0, 2]] / size).astype(np.int64)
	out = []
	order = np.lexsort((key[:, 1], key[:, 0]))
	ks = key[order]
	starts = np.r_[0, np.nonzero(np.any(np.diff(ks, axis=0), axis=1))[0] + 1, len(order)]
	stack = [order[starts[i]:starts[i + 1]] for i in range(len(starts) - 1)]
	while stack:
		sel = stack.pop()
		used, inv = np.unique(t[sel].ravel(), return_inverse=True)
		if len(used) > max_verts:
			c = cent[sel]
			ax = 0 if np.ptp(c[:, 0]) >= np.ptp(c[:, 2]) else 2
			med = np.median(c[:, ax])
			stack += [sel[c[:, ax] <= med], sel[c[:, ax] > med]]
			continue
		tv = v[used]
		center = (tv.min(0) + tv.max(0)) / 2
		out.append((center, (tv - center).astype(np.float32), inv.reshape(-1, 3).astype(np.uint16)))
	return out


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--tile", type=float, default=50)
	ap.add_argument("--radius", type=float, default=150)
	ap.add_argument("--scan", default="gotham_scan.scan")
	ap.add_argument("--physx", default="gotham_here.obj")
	ap.add_argument("--out", default="gotham_tiles")
	a = ap.parse_args()
	scan_path, px_path = os.path.join(LOGS, a.scan), os.path.join(LOGS, a.physx)
	feet = header_batman(scan_path) - np.array([0, 0, HALF_HEIGHT_UU])
	sv, st = scan_mesh(scan_path, feet)
	pv, pt = physx_mesh(px_path, feet, a.radius + 10)
	v = np.concatenate([sv, pv])
	t = np.concatenate([st, pt + len(sv)])
	# drop degenerate triangles
	e1, e2 = v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]]
	t = t[np.linalg.norm(np.cross(e1, e2), axis=1) > 1e-6]
	ts = tiles(v, t, a.tile)
	out = os.path.join(LOGS, a.out + ".awt")
	with open(out, "wb") as f:
		f.write(b"AWT1")
		f.write(struct.pack("<I", len(ts)))
		for center, tv, ti in ts:
			f.write(struct.pack("<3fII", *center.astype(np.float32), len(tv), len(ti)))
			f.write(tv.tobytes())
			f.write(ti.tobytes())
	with open(os.path.join(LOGS, a.out + ".origin"), "w") as f:  # the guest's anchor: file origin in Arkham (feet, UU)
		f.write("%.2f %.2f %.2f\n" % tuple(feet))
	nv = [len(x[1]) for x in ts]
	nt = [len(x[2]) for x in ts]
	print("%d tiles -> %s (%.1f MB); per tile: verts max %d, tris mean %.0f max %d" % (len(ts), out, os.path.getsize(out) / 1e6, max(nv),
	      np.mean(nt), max(nt)))


if __name__ == "__main__":
	main()
