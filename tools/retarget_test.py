"""retarget_test.py [pose index]: offline check of the Batman -> Spider-Man pose transfer.

Takes Batman's pose (logs/ak_skeleton.json, SpaceBases: mesh space, UU, X forward, Y right, Z up) and one recorded
Spider-Man pose (logs/sm_poses.npy: 237 model-space 4x4, rows = rotation rows then translation; +x left, +y up,
+z forward, meters), turns each mapped Spider-Man segment to Batman's direction (shortest arc, the joint's whole
subtree rotating about it, his own bone lengths kept) and draws Batman, Spider-Man and the result as stick figures
to logs/retarget_test.png."""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "logs")


def ak_to_sm(p):
	"""Batman mesh space (UU) -> Spider-Man model space (m)."""
	p = np.asarray(p, np.float64)
	return np.stack([-p[..., 1], p[..., 2], p[..., 0]], -1) / 100.0


def arc(a, b):
	"""Rotation matrix (column vectors) turning unit a onto unit b."""
	v = np.cross(a, b)
	c = float(np.dot(a, b))
	if c < -0.9999:  # opposite: any perpendicular axis
		axis = np.cross(a, [1, 0, 0]) if abs(a[0]) < 0.9 else np.cross(a, [0, 1, 0])
		axis /= np.linalg.norm(axis)
		return 2 * np.outer(axis, axis) - np.eye(3)
	vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
	return np.eye(3) + vx + vx @ vx / (1 + c)


def subtree(parents, root):
	kids = {}
	for j, p in enumerate(parents):
		if j != p: kids.setdefault(int(p), []).append(j)
	out, todo = [], [root]
	while todo:
		j = todo.pop()
		out.append(j)
		todo += kids.get(j, [])
	return out


def retarget(M, P_ak, rmap, min_len=0.03):
	"""M: (237, 4, 4) Spider-Man model-space pose (modified copy returned); P_ak: Batman bone positions in SM space."""
	M = M.copy()
	parents = rmap["sm_parents"]
	subs = {}
	for name in ("spine", "leg_l", "leg_r", "arm_l", "arm_r"):  # trunk first: the limbs hang off it
		ch = rmap["chains"][name]
		for k in range(len(ch["sm"]) - 1):
			a, b = ch["sm"][k], ch["sm"][k + 1]
			A, B = ch["ak_index"][k], ch["ak_index"][k + 1]
			ta, tb = M[a, 3, :3], M[b, 3, :3]
			cur, want = tb - ta, P_ak[B] - P_ak[A]
			if np.linalg.norm(cur) < min_len or np.linalg.norm(want) < 1e-4: continue
			Q = arc(cur / np.linalg.norm(cur), want / np.linalg.norm(want))
			for x in subs.setdefault(a, subtree(parents, a)):
				M[x, :3, :3] = M[x, :3, :3] @ Q.T          # each axis row turned
				M[x, 3, :3] = ta + Q @ (M[x, 3, :3] - ta)   # positions about the joint
	return M


def main():
	rmap = json.load(open(os.path.join(LOGS, "retarget_map.json")))
	ak = json.load(open(os.path.join(LOGS, "ak_skeleton.json")))
	poses = np.load(os.path.join(LOGS, "sm_poses.npy")).astype(np.float64)
	T = poses[:, :, 3, :3]
	f = int(sys.argv[1]) if len(sys.argv) > 1 else int(np.argmax(T[:, 75, 1] - T[:, 10, 1]))  # head high over the hips: upright
	M = poses[f]
	P_ak = ak_to_sm(np.array(ak["space_bases"])[:, 4:7])
	R = retarget(M, P_ak, rmap)

	import matplotlib
	matplotlib.use("Agg")
	import matplotlib.pyplot as plt
	segs_sm = [(j, p) for j, p in enumerate(rmap["sm_parents"]) if j != p]
	core = [n for n in ak["bones"] if n and n.startswith("Bip01")]
	bi = {n: i for i, n in enumerate(ak["bones"])}
	segs_ak = [(bi[n], ak["parents"][bi[n]]) for n in core if ak["parents"][bi[n]] != bi[n]]
	fig, ax = plt.subplots(2, 3, figsize=(12, 8))
	for col, (title, pts, segs) in enumerate((("Batman (converted)", P_ak, segs_ak), ("Spider-Man pose %d" % f, M[:, 3, :3], segs_sm),
	                                         ("Spider-Man retargeted", R[:, 3, :3], segs_sm))):
		for row, (h, v, name) in enumerate(((0, 1, "front (x left, y up)"), (2, 1, "side (z forward, y up)"))):
			a = ax[row, col]
			for c, p in segs:
				a.plot([pts[c, h], pts[p, h]], [pts[c, v], pts[p, v]], "-", lw=1)
			a.set_aspect("equal")
			a.set_title("%s - %s" % (title, name), fontsize=8)
			a.set_xlim(-1, 1)
			a.set_ylim(-0.2, 2.0)
	plt.tight_layout()
	out = os.path.join(LOGS, "retarget_test.png")
	plt.savefig(out, dpi=80)
	print("pose", f, "->", out)


if __name__ == "__main__":
	main()
