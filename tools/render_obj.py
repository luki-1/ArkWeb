"""Render an ArkWeb Gotham export (logs/<name>.obj, UU, Z up) to PNGs: a top-down height map and a
shaded perspective view from behind/above Batman. Groups without faces are convex point sets and
are hulled here. Pure numpy rasterizer, idle priority.

  python render_obj.py ../logs/gotham_150m.obj
"""
import math
import os
import re
import sys

import numpy as np
from scipy.spatial import ConvexHull

import pe_tools  # noqa: F401  (idle priority, one core)


def load(path):
	batman = None
	groups, cur = [], None
	verts = []
	with open(path) as f:
		for line in f:
			if line.startswith("#"):
				m = re.search(r"Batman ([-\d.]+) ([-\d.]+) ([-\d.]+)", line)
				if m: batman = np.array([float(x) for x in m.groups()])
			elif line.startswith("o "):
				cur = {"v0": len(verts), "faces": []}
				groups.append(cur)
			elif line.startswith("v "):
				verts.append([float(x) for x in line.split()[1:4]])
			elif line.startswith("f "):
				cur["faces"].append([int(x) - 1 for x in line.split()[1:4]])
	verts = np.array(verts, dtype=np.float64)
	tris = []
	hulled = 0
	for i, g in enumerate(groups):
		v1 = groups[i + 1]["v0"] if i + 1 < len(groups) else len(verts)
		if g["faces"]:
			tris.extend(g["faces"])
		elif v1 - g["v0"] >= 4:
			try:
				h = ConvexHull(verts[g["v0"]:v1])
				tris.extend((h.simplices + g["v0"]).tolist())
				hulled += 1
			except Exception:
				pass
	return verts, np.array(tris, dtype=np.int64), batman, hulled


def raster(img, zbuf, p2, depth, color):
	"""Fill one screen-space triangle (p2: 3x2) with flat color, nearest depth wins."""
	h, w = zbuf.shape
	x0, y0 = np.floor(p2.min(axis=0)).astype(int)
	x1, y1 = np.ceil(p2.max(axis=0)).astype(int)
	x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, w - 1), min(y1, h - 1)
	if x0 > x1 or y0 > y1: return
	ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
	(ax, ay), (bx, by), (cx, cy) = p2
	den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
	if abs(den) < 1e-9: return
	l1 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
	l2 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
	l3 = 1 - l1 - l2
	inside = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
	if not inside.any(): return
	z = l1 * depth[0] + l2 * depth[1] + l3 * depth[2]
	sub = zbuf[y0:y1 + 1, x0:x1 + 1]
	win = inside & (z < sub)
	sub[win] = z[win]
	img[y0:y1 + 1, x0:x1 + 1][win] = color


def render_view(verts, tris, eye, target, w, h, fov_deg, out):
	fwd = target - eye; fwd /= np.linalg.norm(fwd)
	right = np.cross(fwd, [0, 0, 1.0]); right /= np.linalg.norm(right)
	up = np.cross(right, fwd)
	rel = verts - eye
	cam = np.stack([rel @ right, rel @ up, rel @ fwd], axis=1)
	f = 0.5 * h / math.tan(math.radians(fov_deg) / 2)
	img = np.zeros((h, w, 3), dtype=np.float32) + np.array([0.55, 0.65, 0.8], dtype=np.float32)
	zbuf = np.full((h, w), np.inf)
	light = np.array([0.4, 0.3, 0.85]); light /= np.linalg.norm(light)
	tri_v = verts[tris]
	n = np.cross(tri_v[:, 1] - tri_v[:, 0], tri_v[:, 2] - tri_v[:, 0])
	n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
	shade = 0.35 + 0.65 * np.abs(n @ light)
	height = np.clip((tri_v[:, :, 2].mean(axis=1) - verts[:, 2].min()) / 6000.0, 0, 1)
	order = np.argsort(-cam[tris][:, :, 2].mean(axis=1))
	for t in order:
		c = cam[tris[t]]
		if (c[:, 2] < 50).any(): continue
		p2 = np.stack([w / 2 + f * c[:, 0] / c[:, 2], h / 2 - f * c[:, 1] / c[:, 2]], axis=1)
		if (p2[:, 0] < -w).all() or (p2[:, 0] > 2 * w).all(): continue
		base = np.array([0.55 + 0.35 * height[t], 0.55 + 0.2 * height[t], 0.6 - 0.2 * height[t]])
		raster(img, zbuf, p2, c[:, 2], base * shade[t])
	return img


def main():
	path = sys.argv[1]
	verts, tris, batman, hulled = load(path)
	print("%d vertices, %d triangles (%d convex hulls rebuilt), Batman at %s" % (len(verts), len(tris), hulled, batman))
	import matplotlib
	matplotlib.use("Agg")
	import matplotlib.pyplot as plt
	base = os.path.splitext(path)[0]

	# top-down: highest triangle point per cell
	res, half = 50.0, 15000.0
	n = int(2 * half / res)
	top = np.full((n, n), np.nan)
	for tri in verts[tris]:
		xs = ((tri[:, 0] - batman[0] + half) / res).astype(int)
		ys = ((tri[:, 1] - batman[1] + half) / res).astype(int)
		zx = tri[:, 2].max()
		for gx in range(max(xs.min(), 0), min(xs.max(), n - 1) + 1):
			for gy in range(max(ys.min(), 0), min(ys.max(), n - 1) + 1):
				if np.isnan(top[gy, gx]) or zx > top[gy, gx]: top[gy, gx] = zx
	fig, ax = plt.subplots(figsize=(8, 8), dpi=110)
	im = ax.imshow((top - batman[2]) / 100.0, origin="lower", cmap="viridis", extent=[-150, 150, -150, 150])
	ax.plot(0, 0, "r^", ms=12, label="Batman")
	ax.set_xlabel("m (UE3 X)"); ax.set_ylabel("m (UE3 Y)"); ax.legend(loc="upper right")
	plt.colorbar(im, ax=ax, label="height above Batman (m)", shrink=0.8)
	ax.set_title("Gotham collision within 150 m of Batman (top-down)")
	plt.savefig(base + "_top.png", bbox_inches="tight"); plt.close()

	# perspective: 60 m behind and 35 m above Batman, looking past him
	eye = batman + np.array([-6000.0, -2500.0, 3500.0])
	img = render_view(verts, tris, eye, batman + np.array([2000.0, 800.0, 0.0]), 960, 600, 70, None)
	plt.imsave(base + "_view.png", np.clip(img, 0, 1))
	print("wrote", base + "_top.png", base + "_view.png")


if __name__ == "__main__":
	main()
