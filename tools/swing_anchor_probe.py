"""Find where Spider-Man's webs attach, from outside the game.

Records the SwingPointHunter / HeroRopeManager / HeroStateSwing objects at 10 Hz while the hero
swings (starts once he moves faster than 8 m/s, then 30 s), finds float triples that behave like
web anchors (10-100 m from the hero, constant for a while, then jump to a new spot), and reports how
far each anchor is from the nearest loaded Gotham hull.

  python swing_anchor_probe.py [seconds]
"""
import os
import sys
import time

import numpy as np
from scipy.spatial import ConvexHull

import pe_tools  # noqa: F401  (idle priority)
import gotham_overlay as g
from sm_proc import Proc

VTABLES = {0x38ac210: ("SwingPointHunter", 0xf100), 0x38b3df8: ("HeroRopeManager", 0x1000),
           0x38ce690: ("HeroStateSwing", 0x1000), 0x38ce990: ("HeroStateSwingLocal", 0x1000)}


def find_objects(p):
	targets = np.array([p.exe + v for v in VTABLES], np.uint64)
	objs = []
	for base, size in p.regions():
		for off in range(0, size, 1 << 24):
			b = p.read(base + off, min(1 << 24, size - off))
			if not b: continue
			q = np.frombuffer(b, np.uint64)
			for idx in np.nonzero(np.isin(q, targets))[0]:
				name, n = VTABLES[int(q[idx]) - p.exe]
				objs.append((base + off + int(idx) * 8, name, n))
	return objs


def anchors(series, hero):
	"""series: T x N float32 (one object's memory), hero: T x 3. Yields (offset, segments)."""
	T, N = series.shape
	for o in range(0, N - 2):
		v = series[:, o:o + 3].astype(np.float64)
		if not np.all(np.isfinite(v)): continue
		d = np.linalg.norm(v - hero, axis=1)
		if np.mean((d > 10) & (d < 100)) < 0.3: continue
		step = np.linalg.norm(np.diff(v, axis=0), axis=1)
		moving_hero = np.linalg.norm(np.diff(hero, axis=0), axis=1) > 0.3
		# constant while the hero moves, with a few jumps
		still = (step < 0.01) & moving_hero
		jumps = np.sum(step > 2)
		if still.sum() < 0.5 * moving_hero.sum() or jumps < 2: continue
		segs, start = [], 0
		for t in range(1, T + 1):
			if t == T or step[t - 1] > 0.01:
				if t - start >= 3: segs.append(v[start])
				start = t
		if len(segs) >= 2: yield o * 4, np.array(segs)


def gotham_distance(points):
	fn, limit, origin = g.placement()
	hulls = [h + origin for h in g.load_hulls(os.path.join(g.LOGS, fn), limit)]
	centers = np.array([h.mean(0) for h in hulls])
	out = []
	for pt in points:
		near = np.argsort(np.linalg.norm(centers - pt, axis=1))[:60]
		best = np.inf
		for i in near:
			try:
				eq = ConvexHull(hulls[i]).equations
			except Exception:
				continue
			best = min(best, max(0.0, float((eq[:, :3] @ pt + eq[:, 3]).max())))
		out.append(best)
	return np.array(out)


def main():
	secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
	p = Proc()
	objs = find_objects(p)
	print("objects:", [(hex(a), n) for a, n, _ in objs], flush=True)
	print("waiting for the hero to swing (> 8 m/s)...", flush=True)
	last = p.hero_matrix()[3, :3]
	while len(sys.argv) < 3 or sys.argv[2] != "now":
		time.sleep(0.25)
		cur = p.hero_matrix()[3, :3]
		if np.linalg.norm(cur - last) / 0.25 > 8: break
		last = cur
	print(time.strftime("%H:%M:%S"), "recording %.0f s" % secs, flush=True)
	hero, mem = [], {a: [] for a, _, _ in objs}
	t_end = time.time() + secs
	while time.time() < t_end:
		hero.append(p.hero_matrix()[3, :3].astype(np.float64))
		for a, _, n in objs:
			b = p.read(a, n)
			mem[a].append(np.frombuffer(b, np.float32) if b else np.full(n // 4, np.nan, np.float32))
		time.sleep(0.1)
	hero = np.array(hero)
	np.savez(os.path.join(g.LOGS, sys.argv[3] if len(sys.argv) > 3 else "swing_probe.npz"), hero=hero, **{"m%x" % a: np.array(mem[a]) for a in mem})
	print("path %.0f m" % np.linalg.norm(np.diff(hero, axis=0), axis=1).sum(), flush=True)

	found = []
	for a, name, _ in objs:
		for off, segs in anchors(np.array(mem[a]), hero):
			found.append((name, a, off, segs))
	print("%d anchor-like fields" % len(found), flush=True)
	for name, a, off, segs in found[:12]:
		d = gotham_distance(segs)
		print("%s %x +%x: %d anchors, distance to Gotham (m): %s" % (name, a, off, len(segs), np.round(d, 1)), flush=True)
		for s in segs[:6]: print("     ", np.round(s, 1))


if __name__ == "__main__":
	main()
