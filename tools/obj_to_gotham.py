"""Convert an AK Gotham export (logs/<name>.obj: UU, Z up, header with Batman's position) into the
guest's hull file (logs/<name>.bin): convex point sets in Spider-Man space (meters, Y up), relative
to Batman's feet, nearest first. Boxes (8 vertices) and convex point groups become hulls; the two
big triangle meshes are skipped (they'd need splitting).

Format: "AWG1", u32 count, then per hull u32 n + n * float3.
The axis mapping is coords.h HostDirToGuest for a right-handed guest: guest = (x, z, y) / 100.
"""
import os
import re
import struct
import sys

import numpy as np

import pe_tools  # noqa: F401  (idle priority)

HALF_HEIGHT_UU = 95.0  # Batman's collision cylinder half height


def main():
	src = sys.argv[1]
	dst = os.path.splitext(src)[0] + ".bin"
	batman, groups, verts, cur = None, [], [], None
	with open(src) as f:
		for line in f:
			if line.startswith("#"):
				m = re.search(r"Batman ([-\d.]+) ([-\d.]+) ([-\d.]+)", line)
				if m: batman = np.array([float(x) for x in m.groups()])
			elif line.startswith("o "):
				cur = {"v0": len(verts), "faces": 0}
				groups.append(cur)
			elif line.startswith("v "):
				verts.append([float(x) for x in line.split()[1:4]])
			elif line.startswith("f "):
				cur["faces"] += 1
	verts = np.array(verts)
	feet = batman - np.array([0, 0, HALF_HEIGHT_UU])
	hulls, skipped = [], 0
	thickened = [0]
	for i, g in enumerate(groups):
		v1 = groups[i + 1]["v0"] if i + 1 < len(groups) else len(verts)
		pts = verts[g["v0"]:v1]
		if len(pts) < 4 or len(pts) > 255:
			skipped += 1
			continue
		rel = (pts - feet) / 100.0
		guest = np.stack([rel[:, 0], rel[:, 2], rel[:, 1]], axis=1)  # (x, z, y): Y up, right-handed
		guest = np.unique(np.round(guest, 4), axis=0)
		if len(guest) < 3:
			skipped += 1
			continue
		# Havok's hull builder rejects (near-)flat point sets: give them 10 cm of thickness along their
		# thin axis (downward for floors, so walkable tops stay where they are).
		centered = guest - guest.mean(axis=0)
		_, sv, vt = np.linalg.svd(centered, full_matrices=False)
		if len(guest) < 4 or sv[-1] < 0.02 * max(sv[0], 1e-6) or sv[-1] < 0.01:
			n = vt[-1]
			if n[1] > 0: n = -n  # push down (guest Y is up)
			guest = np.vstack([guest, guest + 0.10 * n])
			thickened[0] += 1
		hulls.append(guest.astype(np.float32))
	hulls.sort(key=lambda h: float(np.sum(h.mean(axis=0) ** 2)))
	with open(dst, "wb") as f:
		f.write(b"AWG1")
		f.write(struct.pack("<I", len(hulls)))
		for h in hulls:
			f.write(struct.pack("<I", len(h)))
			f.write(h.tobytes())
	c = np.array([h.mean(axis=0) for h in hulls])
	print("%d near-flat hulls thickened by 10 cm" % thickened[0])
	print("%d hulls -> %s (%d skipped); centers x %.0f..%.0f, y %.0f..%.0f, z %.0f..%.0f m" % (len(hulls), dst, skipped,
	      c[:, 0].min(), c[:, 0].max(), c[:, 1].min(), c[:, 1].max(), c[:, 2].min(), c[:, 2].max()))
	near = [h for h in hulls if np.linalg.norm(h.mean(axis=0)) < 5.0]
	print("hulls within 5 m of Batman's feet: %d" % len(near))
	for h in near[:3]:
		print("   center %s, %d pts, y range %.2f..%.2f" % (np.round(h.mean(axis=0), 2), len(h), h[:, 1].min(), h[:, 1].max()))


if __name__ == "__main__":
	main()
