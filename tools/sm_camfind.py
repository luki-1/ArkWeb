"""Find Spider-Man's gameplay camera transform from outside the game.

Finds every Camera2::Camera / HeroCameraManager object (by vtable) in the heap, looks for
orthonormal 4x4 matrices inside them and one pointer level down whose position is near the hero,
then watches them while the user turns the camera: the camera is the one that rotates and whose
look direction points at the hero.

  python sm_camfind.py [watch seconds]
"""
import sys
import time

import numpy as np

import pe_tools  # noqa: F401  (idle priority, one core)
from sm_proc import Proc

VTABLES = {0x3871218: "Camera2::Camera", 0x38712a0: "Camera2::CameraCinematic", 0x38b1dd0: "Hero::HeroCameraManager",
           0x38b1b50: "Hero::HeroCameraManagerBase"}
OBJ_BYTES, PTR_BYTES = 0x1000, 0x600


def matrices(buf, base, hero):
	"""(address, 4x4) for each 16-aligned orthonormal row-major matrix with a position near the hero."""
	f = np.frombuffer(buf[:len(buf) // 16 * 16], np.float32).reshape(-1, 4).astype(np.float64)
	if len(f) < 4: return []
	with np.errstate(all="ignore"):
		unit = np.abs(np.linalg.norm(f[:, :3], axis=1) - 1) < 0.01  # per vec4
		ok = unit[:-3] & unit[1:-2] & unit[2:-1]
		i = np.nonzero(ok)[0]
		if not len(i): return []
		r0, r1, r2, pos = f[i, :3], f[i + 1, :3], f[i + 2, :3], f[i + 3, :3]
		ortho = (np.abs((r0 * r1).sum(1)) < 0.01) & (np.abs((r0 * r2).sum(1)) < 0.01) & (np.abs((r1 * r2).sum(1)) < 0.01)
		d = np.linalg.norm(pos - hero, axis=1)
		keep = i[ortho & (d > 0.3) & (d < 30)]
	return [(base + int(k) * 16, f[k:k + 4].astype(np.float32)) for k in keep]


def main():
	secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12
	p = Proc()
	hm = p.hero_matrix()
	if hm is None: sys.exit("no hero transform (is the guest log from this run?)")
	hero = hm[3, :3]
	print("hero at %s" % np.round(hero, 2), flush=True)
	targets = np.array([p.exe + v for v in VTABLES], np.uint64)
	objs, scanned = [], 0
	for base, size in p.regions():
		for off in range(0, size, 1 << 24):
			n = min(1 << 24, size - off)
			b = p.read(base + off, n)
			if not b: continue
			scanned += n
			q = np.frombuffer(b, np.uint64)
			for idx in np.nonzero(np.isin(q, targets))[0]:
				objs.append((base + off + int(idx) * 8, VTABLES[int(q[idx]) - p.exe]))
	print("scanned %.0f MB, %d camera objects" % (scanned / 1e6, len(objs)), flush=True)

	cands = {}
	for addr, cls in objs:
		b = p.read(addr, OBJ_BYTES)
		if not b: continue
		for a, m in matrices(b, addr, hero): cands[a] = (cls, "+%x" % (a - addr))
		q = np.frombuffer(b, np.uint64)
		for k, ptr in enumerate(q):
			ptr = int(ptr)
			if ptr < 0x10000000000 or ptr > 0x7FFFFFFF0000 or ptr & 7: continue
			pb = p.read(ptr, PTR_BYTES)
			if pb:
				for a, m in matrices(pb, ptr, hero): cands.setdefault(a, (cls, "[+%x]+%x" % (k * 8, a - ptr)))
	print("%d matrix candidates near the hero; turn the camera around now (%.0f s)" % (len(cands), secs), flush=True)

	first, maxang, last = {}, {}, {}
	t_end = time.time() + secs
	while time.time() < t_end:
		h = p.hero_matrix()
		hero = h[3, :3] if h is not None else hero
		for a in cands:
			m = p.floats(a, 16)
			if m is None: continue
			m = m.reshape(4, 4)
			if a not in first: first[a] = m
			# largest rotation of any row since the first sample
			ang = np.degrees(np.arccos(np.clip(np.sum(first[a][:3, :3] * m[:3, :3], axis=1), -1, 1))).max()
			maxang[a] = max(maxang.get(a, 0), ang)
			last[a] = (m, hero)
		time.sleep(0.2)

	rows = []
	for a, (cls, where) in cands.items():
		if a not in last: continue
		m, hero = last[a]
		to_hero = hero - m[3, :3]
		dist = np.linalg.norm(to_hero)
		look = m[:3, :3] @ (to_hero / max(dist, 1e-6))  # cosine of each row with the direction to the hero
		rows.append((maxang[a], a, cls, where, dist, look))
	rows.sort(key=lambda r: -r[0])
	print("\nrotated  address           object / offset                       dist   rows . to_hero")
	for ang, a, cls, where, dist, look in rows[:40]:
		print("%6.0f   %x  %-26s %-12s %5.1f m  %s" % (ang, a, cls, where, dist, np.round(look, 2)))


if __name__ == "__main__":
	main()
