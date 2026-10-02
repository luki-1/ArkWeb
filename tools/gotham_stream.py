"""Streams Gotham into Spider-Man around the hero, so the whole city is swingable.

Arkham (the host DLL) scans one 50 m tile at a time with Unreal traces (`tscan`) and exports its PhysX
buildings now and then (`px bexport`, incremental); this script turns each tile into collision (AWT1,
one compressed mesh body per part) and swing hints (AWH1), both in ABSOLUTE guest space (Arkham UU /
100, Y up), and hands them to Spider-Man. Everything travels through the link's shared memory: commands
for Arkham through the host ring, commands and tiles for Spider-Man through the collision ring (in
order); Spider-Man reports back in its GuestState (tiles held and queued, Havok heap left, rescues).

Gotham goes 2000 m ABOVE the spot Spider-Man stands on (--sky): New York's own ledges, swing hints and
everything else it places around the hero are then two kilometers below, out of every search's reach.
(Spider-Man lowers it if his physics world isn't that tall: see gotham.h StreamBegin.)
Once the tiles around the start are built, Spider-Man jumps up onto them (`gotham stream jump`); if
that teleport doesn't hold, the streamer falls back to placing Gotham at his feet.

Start it with both games in the world, Batman on a street and Spider-Man standing still: Batman's feet
become Spider-Man's spot.

  python gotham_stream.py [--sky 2000] [--near 80] [--far 120] [--unload 200] [--resume] [--rescan]

Scans are cached in logs/stream/ (absolute coordinates). A loaded tile whose scan looks incomplete
(mostly water where PhysX has tall buildings: Arkham hadn't loaded that part yet) is scanned once more
when the hero comes close, and a tile is rebuilt when new PhysX buildings for it arrive.
"""
import argparse
import collections
import glob
import os
import re
import struct
import time
import warnings

import numba
import numpy as np
from scipy.spatial import ConvexHull

import pe_tools  # noqa: F401  (sets idle priority; raised to below-normal on one other core below)
import arkweb_proto as proto
from build_gotham_hints import box_obb, hull_obb
from build_gotham_tiles import LOGS, tiles

TILE = 5000.0       # UU (50 m)
STEP = TILE / 12    # UU between scan columns (13 x 13 per tile, edges shared; Arkham traces cost ~0.3 ms)
SCAN_UP, SCAN_DOWN = 30000.0, 30000.0  # column top / bottom relative to the origin's height
PX_RADIUS = 40000.0  # UU, horizontal
WATER_Z = -270.0     # UU: the sea surface where traces do hit it (-267..-273 around the docks)
STREAM = os.path.join(LOGS, "stream")
HINTS_PER_TILE = 150  # the swing hint database holds about 5.7k besides New York's
SMALL_UU = 100.0     # PhysX shapes smaller than this in every direction are left out (props)
BOXED_M = 3.0        # convex shapes smaller than this (guest m) collide as their box
PART_VERTS = 2500    # vertices per compressed-mesh part: each part is one build inside Spider-Man's physics step (6000: up to 23 ms)
BOX_TRIS = np.array([[0, 2, 1], [1, 2, 3], [4, 5, 6], [5, 7, 6], [0, 1, 4], [1, 5, 4], [2, 6, 3], [3, 6, 7], [0, 4, 2], [2, 4, 6], [1, 3, 5],
                     [3, 7, 5]], np.int64)

_k32 = proto._k32


def log(*a):
	print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---- transport -----------------------------------------------------------------------------------
class Channel:
	"""Records for one game through its ring, in order; held back while the ring is full."""

	def __init__(self, link, offset, total):
		self.ring = proto.RingWriter(link, offset, total)
		self.queue = collections.deque()

	def command(self, line):
		self.queue.append((proto.REC_COMMAND, line.encode("ascii")))

	def tile(self, key, awt, awh):
		head = bytearray(proto.TILE_RECORD_BYTES)
		k = key.encode("ascii")[:31]
		head[:len(k)] = k
		struct.pack_into("<II", head, 32, len(awt), len(awh))
		self.queue.append((proto.REC_TILE, bytes(head) + awt + awh))

	def flush(self):
		while self.queue:
			rtype, payload = self.queue[0]
			if not self.ring.write(rtype, payload): return False
			self.queue.popleft()
		return True


# ---- PhysX ----------------------------------------------------------------------------------------
def read_awp(path):
	b = open(path, "rb").read()
	if b[:4] != b"AWP1": raise ValueError("bad export " + path)
	bx, by, bz, rad = struct.unpack_from("<4f", b, 4)
	n, = struct.unpack_from("<I", b, 20)
	off, shapes = 24, []
	for _ in range(n):
		typ, nv, nt = struct.unpack_from("<3I", b, off)
		off += 12
		v = np.frombuffer(b, np.float32, nv * 3, off).reshape(-1, 3).astype(np.float64)
		off += nv * 12
		t = np.frombuffer(b, np.uint32, nt * 3, off).reshape(-1, 3).astype(np.int64)
		off += nt * 12
		if nv and np.all(np.isfinite(v)): shapes.append((typ, v, t))
	return (bx, by, rad), shapes


def uu_to_guest(p):
	p = np.asarray(p, np.float64) / 100.0
	return np.stack([p[..., 0], p[..., 2], p[..., 1]], axis=-1)


def key_of(x, y):
	return int(np.floor(x / TILE)), int(np.floor(y / TILE))


def name_of(k):
	return "t%d_%d" % k


def center_of(k):
	return np.array([(k[0] + 0.5) * TILE, (k[1] + 0.5) * TILE])


@numba.njit(cache=True)
def hull_tris(p):
	"""Triangles (outward) of the convex hull of a small point set (n <= 62): every plane through three
	points with all points on one side is a face; each face (the set of points on it) is fanned once in
	angular order. Brute force, but compiled: about 0.05 ms for the usual 8-30 points, no temp files
	(scipy's qhull opens one per call on Windows)."""
	n = p.shape[0]
	out = np.empty((4 * n + 8, 3), np.int64)
	m = 0
	if n < 3 or n > 62: return out[:0]
	scale = 0.0
	for a in range(n):
		for c in range(3):
			d = abs(p[a, c] - p[0, c])
			if d > scale: scale = d
	if scale <= 0.0: return out[:0]
	eps = 1e-4 * scale + 1e-4
	masks = np.empty(4 * n + 8, np.int64)
	nm = 0
	idx = np.empty(n, np.int64)
	ang = np.empty(n)
	for i in range(n - 2):
		for j in range(i + 1, n - 1):
			ux, uy, uz = p[j, 0] - p[i, 0], p[j, 1] - p[i, 1], p[j, 2] - p[i, 2]
			for k in range(j + 1, n):
				vx, vy, vz = p[k, 0] - p[i, 0], p[k, 1] - p[i, 1], p[k, 2] - p[i, 2]
				nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
				ln = np.sqrt(nx * nx + ny * ny + nz * nz)
				if ln < 1e-9 * scale * scale: continue
				nx, ny, nz = nx / ln, ny / ln, nz / ln
				d = nx * p[i, 0] + ny * p[i, 1] + nz * p[i, 2]
				pos, neg, mask = False, False, 0
				for q in range(n):
					s = nx * p[q, 0] + ny * p[q, 1] + nz * p[q, 2] - d
					if s > eps: pos = True
					elif s < -eps: neg = True
					else: mask |= 1 << q
					if pos and neg: break
				if pos and neg: continue
				seen = False
				for t in range(nm):
					if masks[t] == mask:
						seen = True
						break
				if seen: continue
				if nm >= masks.shape[0]: return out[:m]
				masks[nm] = mask
				nm += 1
				if pos: nx, ny, nz = -nx, -ny, -nz  # outward: every other point is behind the face
				cnt, cx, cy, cz = 0, 0.0, 0.0, 0.0
				for q in range(n):
					if (mask >> q) & 1:
						idx[cnt] = q
						cnt += 1
						cx += p[q, 0]
						cy += p[q, 1]
						cz += p[q, 2]
				cx, cy, cz = cx / cnt, cy / cnt, cz / cnt
				bx, by, bz = p[idx[0], 0] - cx, p[idx[0], 1] - cy, p[idx[0], 2] - cz
				wx, wy, wz = ny * bz - nz * by, nz * bx - nx * bz, nx * by - ny * bx  # w = n x b: (b, w) turns counter-clockwise about n
				for t in range(cnt):
					q = idx[t]
					dx, dy, dz = p[q, 0] - cx, p[q, 1] - cy, p[q, 2] - cz
					ang[t] = np.arctan2(dx * wx + dy * wy + dz * wz, dx * bx + dy * by + dz * bz)
				order = np.argsort(ang[:cnt])
				for t in range(1, cnt - 1):
					if m >= out.shape[0]: return out[:m]
					out[m, 0], out[m, 1], out[m, 2] = idx[order[0]], idx[order[t]], idx[order[t + 1]]
					m += 1
	return out[:m]


class Shapes:
	"""Every PhysX shape the (incremental) exports delivered, in guest space, bucketed by tile: coll (every
	tile a shape overlaps) and hint (the tile of its center). Convex hulls are triangulated only when a
	tile that uses them is built (an export can bring 30k new shapes when Arkham streams a district in;
	doing them all at once stalled the streamer for seconds)."""

	def __init__(self):
		self.verts, self.tris, self.types = [], [], []  # tris: None until needed (convex hulls)
		self.lo, self.hi = [], []  # guest AABBs
		self.coll, self.hint = {}, {}
		self.circles = []  # (x, y, radius) UU of each export
		self.hull_ms = 0.0
		self.seen = set()  # geometric signatures: Arkham re-creates actors when it streams a district back in
		self.duplicates = 0

	def add_export(self, path):
		"""-> number of shapes that were new (not seen before, by geometry)."""
		(cx, cy, radius), shapes = read_awp(path)
		t0 = time.perf_counter()
		self.circles.append((cx, cy, radius))
		if not shapes: return 0
		# per-shape centroid / extent in one go (all vertices concatenated)
		counts = np.array([len(v) for _, v, _ in shapes])
		allv = np.concatenate([v for _, v, _ in shapes])
		starts = np.r_[0, np.cumsum(counts)[:-1]]
		cent = np.add.reduceat(allv, starts) / counts[:, None]
		vmin, vmax = np.minimum.reduceat(allv, starts), np.maximum.reduceat(allv, starts)
		ext = vmax - vmin
		sig_c, sig_e = np.round(cent * 2).astype(np.int64), np.round(ext).astype(np.int64)  # 0.5 cm / 1 cm
		gall = uu_to_guest(allv)
		added = 0
		for n, (typ, v, t) in enumerate(shapes):
			sig = (typ, int(counts[n]), *sig_c[n], *sig_e[n])
			if sig in self.seen:
				self.duplicates += 1
				continue
			self.seen.add(sig)
			if ext[n].max() < SMALL_UU: continue  # props too small to matter for traversal
			g = gall[starts[n]:starts[n] + counts[n]]
			si = len(self.verts)
			self.verts.append(g)
			self.tris.append(None if typ == 2 else np.asarray(t, np.int64))
			self.types.append(typ)
			self.lo.append(g.min(0))
			self.hi.append(g.max(0))
			ck = key_of(cent[n, 0], cent[n, 1])
			self.hint.setdefault(ck, []).append(si)
			k0, k1 = key_of(vmin[n, 0], vmin[n, 1]), key_of(vmax[n, 0], vmax[n, 1])
			if (k1[0] - k0[0] + 1) * (k1[1] - k0[1] + 1) > 400: k0 = k1 = ck  # absurdly large: keep it in one tile
			for i in range(k0[0], k1[0] + 1):  # big shapes go to every tile they overlap (duplicates collide fine)
				for j in range(k0[1], k1[1] + 1):
					self.coll.setdefault((i, j), []).append(si)
			added += 1
		self.hull_ms = (time.perf_counter() - t0) * 1000
		return added

	def tris_of(self, si):
		t = self.tris[si]
		if t is None:
			g = self.verts[si]
			lo, hi = self.lo[si], self.hi[si]
			if (hi - lo).max() < BOXED_M:
				# a small convex piece is a box: 12 triangles instead of a hull's 20-120 (dense districts have
				# thousands of them per tile, and every triangle is collision Spider-Man has to build) - its own
				# when it is one, else an upright one around it: the axis-aligned box made a diagonal brace or a
				# rotated plank a solid block (2026-10-02)
				obb = None if self.types[si] == 1 and len(g) == 8 else hull_obb(g)
				if obb is not None:
					rows, c, half = obb
					g = np.array([c + sum((((k >> a) & 1) * 2 - 1) * half[a] * rows[a] for a in range(3)) for k in range(8)])
				elif not (self.types[si] == 1 and len(g) == 8):
					g = np.array([[(lo, hi)[k & 1][0], (lo, hi)[(k >> 1) & 1][1], (lo, hi)[(k >> 2) & 1][2]] for k in range(8)])
				self.verts[si] = g
				t = np.asarray(hull_tris(g), np.int64)
				self.tris[si] = t
				return t
			if len(g) <= 62:
				t = hull_tris(g)
			else:
				try:
					t = ConvexHull(g).simplices
				except Exception:
					t = np.zeros((0, 3), np.int64)
			t = np.asarray(t, np.int64)
			self.tris[si] = t
		return t

	def tall_count(self, k, height=5.0):
		return sum(1 for si in self.coll.get(k, ()) if self.hi[si][1] - self.lo[si][1] > height)

	def covers(self, k):
		x, y = center_of(k)
		return any(np.hypot(x - cx, y - cy) + TILE * 0.75 < r for cx, cy, r in self.circles)

	def count(self, k):
		return len(self.coll.get(k, ()))


# ---- tiles --------------------------------------------------------------------------------------------
def read_scan(path):
	"""-> (columns {(i, j): [(x, y, z) highest first]}, half spacing, (x0, y0, nx, ny)) in UU. Columns that
	hit nothing are open water (Arkham's water mostly doesn't block traces): they get a surface at the
	water level, so the hero lands on the sea instead of falling forever."""
	hdr = open(path).readline()
	h = float(re.search(r"step ([\d.]+)", hdr).group(1)) / 2
	m = re.search(r"rect ([-\d.]+) ([-\d.]+), columns (\d+) x (\d+)", hdr)
	with warnings.catch_warnings():
		warnings.simplefilter("ignore")  # an all-water tile has no rows
		d = np.loadtxt(path, comments="#", ndmin=2)
	cols = {}
	for i, j, x, y, z in d[:, :5] if d.size else ():
		cols.setdefault((int(i), int(j)), []).append((x, y, z))
	grid = None
	if m:
		x0, y0, nx, ny = float(m.group(1)), float(m.group(2)), int(m.group(3)), int(m.group(4))
		grid = (x0, y0, nx, ny)
		for i in range(nx):
			for j in range(ny):
				if (i, j) not in cols: cols[(i, j)] = [(x0 + i * 2 * h, y0 + j * 2 * h, WATER_Z)]
	for c in cols.values(): c.sort(key=lambda s: -s[2])
	# (the sea under piers and decks is one plate over the whole tile, made by tile_scan_mesh: as an extra
	# surface at the bottom of every column it made every top look thin, so every scan wall became a 1.5 m
	# fascia and the hero walked under them into buildings, 2026-10-02)
	return cols, h, grid


def tile_scan_mesh(cols, h, tops=None, origin=(0, 0)):
	"""Floor triangles and walls (guest space) from one tile's columns (tops: {global column: top height}
	of this tile and its neighbours, origin: this tile's first global column - for what spans tiles). Neighbouring surfaces within 1.5 m
	are joined; where the neighbour has no surface at that height but a higher one (a building stands
	there), the floor is carried under it, so streets reach the walls with no gap. A surface that joins
	nothing at all (a deck one column wide: a bridge, a walkway) gets a small plate of its own. Walls go
	between neighbours whose tops differ by more than 1 m, unless the taller column also has a surface
	at the lower height (an overpass). Returns (verts, tris, walls as (W, 4, 3) quads)."""
	if not cols: return np.zeros((0, 3)), np.zeros((0, 3), np.int64), np.zeros((0, 4, 3))
	verts, index, used = [], {}, set()

	def vid(key, x, y, z):
		k = (key, round(z, 1))
		if k not in index:
			index[k] = len(verts)
			verts.append((x, y, z))
		return index[k]

	# Floors per grid cell (four columns): every height any of its corners saw - so a floor is not lost
	# when the cell's first column stopped at a solid above it (a pillar through a multi-storey deck: the
	# hero fell through such holes, 2026-10-01). Each corner gives its own surface at that height, or the
	# floor carried under a taller column, or nothing (open edge).
	tris = []
	keys = list(cols)
	i0, i1 = min(k[0] for k in keys), max(k[0] for k in keys)
	j0, j1 = min(k[1] for k in keys), max(k[1] for k in keys)
	for i in range(i0, i1):
		for j in range(j0, j1):
			quad = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
			heights = sorted({round(s[2], 1) for q in quad for s in cols.get(q, ())}, reverse=True)
			done = []
			for z in heights:
				if any(abs(z - d) <= 150 for d in done): continue  # the same floor seen by another corner
				done.append(z)
				real = 0
				vs = []
				for q in quad:
					c = cols.get(q)
					m = None
					if c:
						best = None
						for t in c:
							if abs(t[2] - z) <= 150 and (best is None or abs(t[2] - z) < abs(best[2] - z)): best = t
						if best:
							m = vid(q, *best)
							real += 1
							used.add((q, round(best[2], 1)))
						elif c[0][2] > z + 150:
							m = vid(q, c[0][0], c[0][1], z)  # under a taller column: carry the floor on
					vs.append(m)
				if not real: continue
				a, b, e, cc = vs
				if a is not None and b is not None and e is not None: tris.append((a, b, e))
				if a is not None and e is not None and cc is not None: tris.append((a, e, cc))
				if e is None and a is not None and b is not None and cc is not None: tris.append((a, b, cc))
				if a is None and b is not None and e is not None and cc is not None: tris.append((b, e, cc))
	q = 0.7 * h
	iso = {}
	for (i, j), surfaces in cols.items():
		for s in surfaces:
			if ((i, j), round(s[2], 1)) in used: continue
			iso.setdefault((i, j), []).append(s)
			p = len(verts)
			verts.extend([(s[0] - q, s[1] - q, s[2]), (s[0] + q, s[1] - q, s[2]), (s[0] + q, s[1] + q, s[2]), (s[0] - q, s[1] + q, s[2])])
			tris += [(p, p + 1, p + 2), (p, p + 2, p + 3)]
	# Such plates in neighbouring columns at about the same height are one thing one column wide - a monorail
	# track, a walkway: a strip as wide as the plates joins them (their gaps of 1.3 m let the hero drop
	# through the track, 2026-10-02).
	for (i, j), ss in iso.items():
		for di, dj in ((1, 0), (0, 1), (1, 1), (1, -1)):
			for s in ss:
				for s2 in iso.get((i + di, j + dj), ()):
					if abs(s[2] - s2[2]) > 150.0: continue
					dx, dy = s2[0] - s[0], s2[1] - s[1]
					n = q / max(1e-6, np.hypot(dx, dy))
					ox, oy = -dy * n, dx * n
					p = len(verts)
					verts.extend([(s[0] + ox, s[1] + oy, s[2]), (s2[0] + ox, s2[1] + oy, s2[2]), (s2[0] - ox, s2[1] - oy, s2[2]), (s[0] - ox, s[1] - oy, s[2])])
					tris += [(p, p + 1, p + 2), (p, p + 2, p + 3)]
	# The sea under everything, one plate over the tile: a column's scan stops at a pier, bridge or deck (the
	# water below mostly doesn't block traces), so walking on the water under one fell through (2026-10-02).
	xs, ys = [c[0][0] for c in cols.values()], [c[0][1] for c in cols.values()]
	p = len(verts)
	verts.extend([(min(xs), min(ys), WATER_Z), (max(xs), min(ys), WATER_Z), (max(xs), max(ys), WATER_Z), (min(xs), max(ys), WATER_Z)])
	tris += [(p, p + 1, p + 2), (p, p + 2, p + 3)]
	# Walls between neighbours whose tops differ, down the taller column's surfaces above the lower top: the
	# scan restarts 4 m under each hit and stops inside solids, so a surface with another one under it has
	# open air under it at most 4 m down - an overhang, a deck: a 1.5 m fascia under it - while the lowest
	# one stands on solid: a full wall from the lower top (2026-10-02: walls were only top fascias and the
	# hero walked into buildings). A top one column wide across the wall (a monorail track, a sign, a cable)
	# or part of something thin (a neighbour sees the same top with air under it) gets a fascia too: those
	# made invisible walls down to the street, 2026-10-01.
	def lower(k, z):
		return k in cols and cols[k][0][2] < z - 300.0

	def thin_top(k, z):
		return k in cols and len(cols[k]) > 1 and abs(cols[k][0][2] - z) <= 150.0

	# A building, not a deck, a track or a sign: the columns at least this tall that join the tall one make
	# a block over most of the 5 x 5 around it (roofs may step up; a monorail between buildings is cut off
	# from them by the streets). Arkham's buildings are often hollow shells - the scan hits the roof,
	# restarts inside and hits the floor at the street's height - which reads like an overpass, so they got
	# no walls and the hero swung through them (a third of all steps, 2026-10-02). Vertical traces can't
	# see a facade; such a block that high is one.
	def top_at(g):
		if tops is not None: return tops.get((origin[0] + g[0], origin[1] + g[1]))
		c = cols.get(g)
		return c[0][2] if c else None

	def building(k, z):
		present = sum(top_at((k[0] + a, k[1] + b)) is not None for a in range(-2, 3) for b in range(-2, 3))
		block, todo = {k}, [k]
		while todo:
			c = todo.pop()
			for a in (-1, 0, 1):
				for b in (-1, 0, 1):
					q = (c[0] + a, c[1] + b)
					if q in block or abs(q[0] - k[0]) > 2 or abs(q[1] - k[1]) > 2: continue
					t = top_at(q)
					if t is not None and t >= z - 150.0:
						block.add(q)
						todo.append(q)
		return len(block) >= max(6, 0.52 * present)

	walls = []
	for (i, j), surfaces in cols.items():
		top = surfaces[0]
		for nb, axis in (((i + 1, j), 0), ((i, j + 1), 1)):
			if nb not in cols: continue
			ntop = cols[nb][0]
			lo, hi = sorted((top[2], ntop[2]))
			if hi - lo < 100.0: continue
			tk = (i, j) if top[2] > ntop[2] else nb
			low = nb if tk == (i, j) else (i, j)
			tall = cols[tk]
			mx, my = (top[0] + ntop[0]) / 2, (top[1] + ntop[1]) / 2
			if axis == 0: p0, p1 = (mx, my - h), (mx, my + h)
			else: p0, p1 = (mx - h, my), (mx + h, my)
			if hi - lo >= 800.0 and building(tk, hi):  # a building's facade, whatever the scan saw inside
				walls.append([(p0[0], p0[1], lo), (p1[0], p1[1], lo), (p1[0], p1[1], hi), (p0[0], p0[1], hi)])
				continue
			if any(abs(s[2] - lo) < 150.0 for s in tall[1:]): continue  # an overpass: open under it
			dx, dy = tk[0] - low[0], tk[1] - low[1]
			beyond = (tk[0] + dx, tk[1] + dy)  # the tall column's other neighbour on this axis
			perp = ((tk[0] + dy, tk[1] + dx), (tk[0] - dy, tk[1] - dx))
			ring = [(tk[0] + a, tk[1] + b) for a in (-1, 0, 1) for b in (-1, 0, 1) if a or b]
			spike = lower(beyond, hi) or (beyond not in cols and all(lower(p, hi) for p in perp)) or any(thin_top(r, hi) for r in ring)
			above = [s[2] for s in tall if s[2] > lo + 100.0]
			for n, z in enumerate(above):
				solid = n == len(above) - 1 and len(above) == len(tall) and not (n == 0 and spike)
				z0 = lo if solid else max(lo, z - 150.0)
				walls.append([(p0[0], p0[1], z0), (p1[0], p1[1], z0), (p1[0], p1[1], z), (p0[0], p0[1], z)])
	v = uu_to_guest(np.array(verts))
	w = uu_to_guest(np.array(walls)) if walls else np.zeros((0, 4, 3))
	return v, np.array(tris, np.int64).reshape(-1, 3), w


def scan_hints(cols, h, grid, ground_uu):
	"""Swing hints for buildings the scan saw (PhysX has only some of them): the tile's columns whose top
	is at least 6 m above the ground, merged greedily into rectangles of similar height (6 m bands), one
	upright box each, from the ground to the rectangle's lowest top. -> [(rows, center, half)] guest."""
	if not grid: return []
	x0, y0, nx, ny = grid
	top = np.full((nx, ny), np.nan)
	for (i, j), s in cols.items():
		if 0 <= i < nx and 0 <= j < ny: top[i, j] = s[0][2]
	tall = np.nan_to_num(top - ground_uu, nan=0.0) >= 600
	level = np.where(tall, np.floor(np.nan_to_num(top - ground_uu) / 600), -1).astype(int)
	used = np.zeros_like(tall)
	out = []
	for i in range(nx):
		for j in range(ny):
			if not tall[i, j] or used[i, j]: continue
			lv = level[i, j]
			j1 = j
			while j1 + 1 < ny and tall[i, j1 + 1] and not used[i, j1 + 1] and level[i, j1 + 1] == lv: j1 += 1
			i1 = i
			while i1 + 1 < nx and np.all(tall[i1 + 1, j:j1 + 1] & ~used[i1 + 1, j:j1 + 1] & (level[i1 + 1, j:j1 + 1] == lv)): i1 += 1
			used[i:i1 + 1, j:j1 + 1] = True
			zt = np.nanmin(top[i:i1 + 1, j:j1 + 1])
			xa, xb = x0 + i * 2 * h - h, x0 + i1 * 2 * h + h
			ya, yb = y0 + j * 2 * h - h, y0 + j1 * 2 * h + h
			c = uu_to_guest(np.array([(xa + xb) / 2, (ya + yb) / 2, (ground_uu + zt) / 2]))
			half = np.array([(xb - xa) / 200, (zt - ground_uu) / 200, (yb - ya) / 200])
			out.append((np.eye(3), c, half))  # rows x, up, z: z = x cross up, as hull_obb
	return out


_scans = {}  # path -> (mtime, read_scan result)


def cached_scan(path):
	try:
		mt = os.path.getmtime(path)
	except OSError:
		return None
	c = _scans.get(path)
	if not c or c[0] != mt:
		if len(_scans) > 400: _scans.clear()
		c = _scans[path] = (mt, read_scan(path))
	return c[1]


def neighbour_tops(k):
	"""{global column (tile * 12 + i): top height} over the tile and its eight neighbours' scans (those
	there are): a building on a tile edge is one roof to the facade test."""
	tops = {}
	for dx in (-1, 0, 1):
		for dy in (-1, 0, 1):
			kk = (k[0] + dx, k[1] + dy)
			s = cached_scan(os.path.join(STREAM, name_of(kk) + ".scan"))
			if not s: continue
			for (i, j), c in s[0].items():
				tops.setdefault((kk[0] * 12 + i, kk[1] * 12 + j), c[0][2])
	return tops


def build_tile(k, scan_path, shapes):
	"""-> (awt bytes, awh bytes, tris, hints)"""
	cols, h, grid = read_scan(scan_path)
	sv, st, walls = tile_scan_mesh(cols, h, neighbour_tops(k), (k[0] * 12, k[1] * 12))
	ids = shapes.coll.get(k, [])
	# scan walls only where PhysX has no building of that height (its own faces are exact; a scan wall
	# sits up to half a column spacing in front of the real facade)
	if len(walls) and ids:
		lo, hi = np.array([shapes.lo[i] for i in ids]), np.array([shapes.hi[i] for i in ids])
		tall = hi[:, 1] - lo[:, 1] > 2
		lo, hi = lo[tall], hi[tall]
		mid = walls.mean(1)
		wy0, wy1 = walls[:, :, 1].min(1), walls[:, :, 1].max(1)
		inside = ((mid[:, None, 0] > lo[None, :, 0] - 3) & (mid[:, None, 0] < hi[None, :, 0] + 3) & (mid[:, None, 2] > lo[None, :, 2] - 3) &
			(mid[:, None, 2] < hi[None, :, 2] + 3) & (hi[None, :, 1] > wy1[:, None] - 2) & (lo[None, :, 1] < wy0[:, None] + 2))
		walls = walls[~inside.any(1)]
	vs, ts, base = [sv], [st], len(sv)
	if len(walls):
		vs.append(walls.reshape(-1, 3))
		q = base + 4 * np.arange(len(walls))[:, None]
		ts.append(np.concatenate([q + [0, 1, 2], q + [0, 2, 3]]))
		base += 4 * len(walls)
	# A shape that reaches past the tile (a road, a long facade) is listed in every tile it overlaps: each
	# keeps only the triangles over its own square (+1 m), so no tile carries a whole street - Spider-Man's
	# Havok heap is small and ran out when big meshes were copied into every tile they touched.
	x0, z0 = k[0] * TILE / 100.0, k[1] * TILE / 100.0
	x1, z1, margin = x0 + TILE / 100.0, z0 + TILE / 100.0, 1.0
	for si in ids:
		tt = shapes.tris_of(si)
		if not len(tt): continue
		g = shapes.verts[si]
		lo, hi = shapes.lo[si], shapes.hi[si]
		if lo[0] < x0 - margin or hi[0] > x1 + margin or lo[2] < z0 - margin or hi[2] > z1 + margin:
			tx, tz = g[tt, 0], g[tt, 2]
			keep = (tx.max(1) >= x0 - margin) & (tx.min(1) <= x1 + margin) & (tz.max(1) >= z0 - margin) & (tz.min(1) <= z1 + margin)
			if not keep.any(): continue
			tt = tt[keep]
			used = np.unique(tt)
			remap = np.full(len(g), -1, np.int64)
			remap[used] = np.arange(len(used))
			g, tt = g[used], remap[tt]
		vs.append(g)
		ts.append(tt + base)
		base += len(g)
	v = np.concatenate(vs) if base else np.zeros((0, 3))
	t = np.concatenate(ts) if base else np.zeros((0, 3), np.int64)
	if len(t):
		e1, e2 = v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]]
		t = t[np.linalg.norm(np.cross(e1, e2), axis=1) > 1e-6]
	if len(t):
		# the same triangle twice (overlapping exports of one building, a shape re-created a hair apart): once
		q = np.round(v[t] * 50).astype(np.int64)  # 2 cm
		q.sort(axis=1)
		_, first = np.unique(q.reshape(len(t), 9), axis=0, return_index=True)
		t = t[np.sort(first)]
	# small parts: a compressed mesh build needs several times its size in temporary Havok heap, and each
	# part is built in one go inside a physics step of Spider-Man's (a big one is a visible hitch)
	parts = tiles(v, t, TILE / 100.0, max_verts=PART_VERTS) if len(t) else []
	awt = [b"AWT1", struct.pack("<I", len(parts))]
	for center, tv, ti in parts:
		awt += [struct.pack("<3fII", *center.astype(np.float32), len(tv), len(ti)), tv.tobytes(), ti.tobytes()]
	# swing hints: PhysX buildings centered in this tile, then the scan's own buildings
	lows = [s[-1][2] for s in cols.values()]
	ground_uu = float(np.percentile(lows, 20)) if lows else WATER_Z
	ground = ground_uu / 100
	hints = []
	for si in shapes.hint.get(k, ()):
		p, typ = shapes.verts[si], shapes.types[si]
		if len(p) < 4 or np.ptp(p[:, 1]) < 4: continue
		obb = box_obb(p) if typ == 1 and len(p) == 8 else hull_obb(p)
		if obb is None: continue
		rows, center, half = obb
		if 2 * min(half[0], half[2]) < 2 or center[1] + half[1] < ground + 2: continue
		hints.append((rows, center, half))
	hints.sort(key=lambda hh: -np.prod(hh[2]))
	hints = hints[:HINTS_PER_TILE // 2]
	sh = scan_hints(cols, h, grid, ground_uu)
	sh.sort(key=lambda hh: -np.prod(hh[2]))
	hints += sh[:HINTS_PER_TILE - len(hints)]
	awh = [b"AWH1", struct.pack("<I", len(hints))]
	for rows, center, half in hints:
		awh.append(np.concatenate([np.asarray(rows).ravel(), center, half]).astype(np.float32).tobytes())
	water = sum(1 for c in cols.values() if len(c) == 1 and c[0][2] == WATER_Z) / max(1, len(cols))
	return b"".join(awt), b"".join(awh), sum(len(p[2]) for p in parts), len(hints), water


# ---- main ---------------------------------------------------------------------------------------------
def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--near", type=float, default=80, help="m: tiles this close to the hero are loaded")
	ap.add_argument("--far", type=float, default=120, help="m: ... and this close to where he will be in --lookahead s")
	ap.add_argument("--unload", type=float, default=170, help="m: tiles farther than this are unloaded")
	ap.add_argument("--budget", type=float, default=3, help="ms of Arkham's frame for scanning (doubled while the hero's area is missing)")
	ap.add_argument("--lookahead", type=float, default=2.5, help="s of velocity added to the hero's position")
	ap.add_argument("--inflight", type=int, default=4, help="tile scans queued in Arkham at once")
	ap.add_argument("--sky", type=float, default=2000, help="m Gotham sits above New York (0: at Spider-Man's feet)")
	ap.add_argument("--min-heap", type=float, default=120, help="MB of Havok heap below which far tiles are dropped")
	ap.add_argument("--max-tris", type=float, default=600, help="thousand triangles Spider-Man may hold: past it the farthest tiles go first")
	ap.add_argument("--minutes", type=float, default=180)
	ap.add_argument("--rescan", action="store_true", help="ignore cached scans")
	ap.add_argument("--resume", action="store_true", help="restart without a new begin: keep the guest's placement, reload tiles in place")
	a = ap.parse_args()
	_k32.SetPriorityClass(_k32.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL: idle would starve next to two games
	ncpu = os.cpu_count() or 1
	_k32.SetProcessAffinityMask(_k32.GetCurrentProcess(), 1 << (ncpu - 1))
	os.makedirs(STREAM, exist_ok=True)
	for f in glob.glob(os.path.join(STREAM, "*.part")) + glob.glob(os.path.join(STREAM, "px*.awp")): os.remove(f)
	if a.rescan:
		for f in glob.glob(os.path.join(STREAM, "*.scan")): os.remove(f)

	link = proto.Link()
	ak = Channel(link, proto.OFF_HOST_RING, proto.HOST_RING_BYTES)
	sm = Channel(link, proto.OFF_COLLISION_RING, proto.COLLISION_RING_BYTES)

	def states():
		hs = link.read_slot(proto.OFF_HOST, proto.HOST)
		gs = link.read_slot(proto.OFF_GUEST, proto.GUEST)
		ok = hs and gs and link.alive("host") and link.alive("guest")
		return (hs, gs) if ok else (None, None)

	sky = a.sky
	if a.resume:
		log("resuming: waiting for both games ...")
		while True:
			hs, gs = states()
			if gs and gs["flags"] & proto.GUEST_ANCHORED: break
			time.sleep(0.5)
		g = gs["heroPos"]
		origin = np.array([g[0] * 100, g[2] * 100, g[1] * 100])
		jumped = True
		log("resumed at the hero, %s UU" % np.round(origin))
		# tiles an earlier streamer left in Spider-Man that this one doesn't know: the far ones go now (the near
		# ones are replaced as this one builds them)
		c = key_of(origin[0], origin[1])
		n_unload = 0
		for i in range(c[0] - 40, c[0] + 41):
			for j in range(c[1] - 40, c[1] + 41):
				if np.hypot(*(center_of((i, j)) - origin[:2])) > 30000:
					sm.command("gotham stream unload %s" % name_of((i, j)))
					n_unload += 1
		sm.flush()
		log("asked Spider-Man to drop any tiles farther than 300 m (%d keys)" % n_unload)
	else:
		log("waiting for both games in the world, Spider-Man standing still ...")
		still_since, last_hero = None, None
		while True:
			hs, gs = states()
			if hs and hs["flags"] & proto.HOST_IN_GAME and gs["flags"] & proto.GUEST_IN_WORLD:
				hh = np.array(gs["heroPos"])
				if last_hero is None or np.linalg.norm(hh - last_hero) > 0.05: still_since = time.time()
				last_hero = hh
				if time.time() - still_since > 2: break
			else:
				still_since, last_hero = None, None
			time.sleep(0.2)
		p = hs["puppetPos"]
		origin = np.array([p[0] * 100, p[2] * 100, p[1] * 100])  # Batman's feet, UU
		sm.command("gotham filter off")
		sm.command("gotham stream reset")
		sm.command("gotham slot4 off")
		sm.command("gotham stream begin %.1f %.1f %.1f %.0f" % (origin[0], origin[1], origin[2], sky))
		sm.flush()
		jumped = sky <= 0
		log("origin: Batman's feet %s UU -> Spider-Man's spot%s" % (np.round(origin), " + %.0f m (sky)" % sky if sky > 0 else ""))
	top, bottom = origin[2] + SCAN_UP, origin[2] - SCAN_DOWN

	ak.command("px bforget")  # exports are incremental from here on
	ak.flush()
	log("compiling the hull builder ...")
	hull_tris(np.random.rand(8, 3))

	shapes = Shapes()
	loaded = {}     # key -> {"shapes": PhysX shape count at build, "built": time, "suspect": scan looks incomplete}
	scanning = {}   # key -> (time sent, scan mtime before)
	rescanned = set()
	scan_dist = {}  # key -> UU from the hero when this session's scan of it was asked for (none: an older session's)
	rescans, scan_asked = collections.Counter(), {}  # this session: rescans of a tile, when its scan was last asked for
	trace = open(os.path.join(LOGS, "hero_trace.csv"), "w")  # wall-clock time -> where the hero is (for videos)
	trace.write("time,x,y,z,speed\n")
	px_pending, px_n, px_at, px_time = None, 0, None, 0.0
	filter_on, warned, budget, jump_sent = jumped and a.resume, False, None, 0.0
	last_tris, evicted = {}, {}  # a tile's triangles when last built; when the budget last pushed a tile out
	pos_prev, t_prev, vel = None, None, np.zeros(2)
	stats = collections.Counter()
	last_status, end = 0, time.time() + 60 * a.minutes
	near_uu, far_uu, unload_base = a.near * 100, a.far * 100, a.unload * 100
	if a.resume: sm.command("gotham filter on")

	def scan_path(k):
		return os.path.join(STREAM, name_of(k) + ".scan")

	def scan_mtime(k):
		try:
			return os.path.getmtime(scan_path(k))
		except OSError:
			return 0.0

	while time.time() < end:
		now = time.time()
		hs, gs = states()
		if not hs:
			time.sleep(0.5)
			continue
		g = gs["heroPos"] if gs["flags"] & proto.GUEST_ANCHORED else hs["puppetPos"]
		pos = np.array([g[0] * 100, g[2] * 100, g[1] * 100])
		if pos_prev is not None and now > t_prev:
			v = (pos[:2] - pos_prev[:2]) / (now - t_prev)
			vel = vel * 0.7 + v * 0.3 if np.linalg.norm(v) < 20000 else vel  # ignore teleports
		pos_prev, t_prev = pos, now
		trace.write("%.3f,%.0f,%.0f,%.0f,%.1f\n" % (now, pos[0], pos[1], pos[2], np.linalg.norm(vel) / 100))
		ahead = pos[:2] + vel * a.lookahead
		# 0 too means unknown: a guest that misreads its heap (hkThreadMemory) reports 0 from the start
		heap = gs["heapFreeMB"] if gs["heapFreeMB"] not in (0xFFFFFFFF, 0) else None
		# low Havok heap: keep only what is close
		unload_uu = unload_base if heap is None or heap > a.min_heap else max(near_uu + 3000, unload_base * max(0.0, (heap - 48) / (a.min_heap - 48)))

		# PhysX export (only new shapes): at the start, then every few seconds while the hero moves
		if px_pending:
			if os.path.exists(px_pending):
				try:
					n = shapes.add_export(px_pending)
					if n: log("PhysX export %d: %d new shapes (%d kept, %d duplicates so far, hulls %.0f ms)" % (px_n, n, len(shapes.verts), shapes.duplicates,
					                                                                                           shapes.hull_ms))
				except Exception as e:
					log("PhysX export unreadable: %r" % e)
				os.remove(px_pending)
				px_pending = None
		elif px_at is None or (now - px_time > 3 and np.hypot(*(pos[:2] - px_at)) > 3000):
			px_n += 1
			px_at, px_time = pos[:2].copy(), now
			ak.command("px bexport %.0f px%d" % (PX_RADIUS, px_n))
			px_pending = os.path.join(STREAM, "px%d.awp" % px_n)

		# wanted tiles: around the hero and around where he is heading; nearest first
		r = int(np.ceil((max(near_uu, far_uu) + np.linalg.norm(vel) * a.lookahead) / TILE)) + 1
		c0 = key_of(pos[0], pos[1])
		want = []
		for i in range(c0[0] - r, c0[0] + r + 1):
			for j in range(c0[1] - r, c0[1] + r + 1):
				cx, cy = center_of((i, j))
				d_now = np.hypot(cx - pos[0], cy - pos[1])
				d_ahead = np.hypot(cx - ahead[0], cy - ahead[1])
				if d_now < near_uu or (d_ahead < far_uu and d_now < unload_uu - TILE): want.append((min(d_now, d_ahead), (i, j)))
		want.sort()

		# scans: new tiles; and once, close by, a loaded tile whose scan may be missing things - Arkham only
		# has the district around Batman loaded, so a scan from far away (or from an earlier session) can miss
		# roads: columns that hit nothing (water, -270 UU) where there is a street (the hero fell through one,
		# 2026-10-01); also mostly water where PhysX has tall buildings
		for d, k in want:
			if len(scanning) >= a.inflight: break
			if k in scanning: continue
			cached = scan_mtime(k)
			info = loaded.get(k)
			far_scan = scan_dist.get(k, 1e9) > 8000 and info is not None and info.get("water", 0) > 0.02
			# columns that hit nothing in a city tile (PhysX has things there: not the bay): Arkham hadn't
			# streamed that district in yet when it was scanned - this session swinging past fast, or an earlier
			# one (146 of 169 empty; he fell through roofs and floors there, 2026-10-02). Again from 90 m on, so
			# it is done before he gets there, up to 4 times at growing gaps: a scan while Arkham is still
			# loading the district comes back as empty as before
			water = info.get("water", 0) if info else 0
			empty = (info is not None and water > 0.10 and shapes.count(k) >= 10 and d < 9000 and rescans[k] < 4 and
			         now - scan_asked.get(k, 0) > (10, 20, 40, 60)[rescans[k]])
			if cached and not (empty or info and (info.get("suspect") and d < 6000 or far_scan and d < 9000) and k not in rescanned): continue
			scan_dist[k] = np.hypot(*(center_of(k) - pos[:2]))
			scan_asked[k] = now
			if cached:
				stats["rescans"] += 1
				rescanned.add(k)
				rescans[k] += 1
			x0, y0 = k[0] * TILE, k[1] * TILE
			ak.command("tscan %s %.0f %.0f %.0f %.0f %.3f %.0f %.0f 66" % (name_of(k), x0, y0, x0 + TILE, y0 + TILE, STEP, top, bottom))
			scanning[k] = (now, cached)
		for k, (t, before) in list(scanning.items()):
			if scan_mtime(k) > before:
				del scanning[k]
				stats["scanned"] += 1
				if k in loaded: loaded[k]["stale"] = True  # rebuild with the new scan
			elif now - t > 30:
				log("scan %s timed out - asking again" % name_of(k))
				del scanning[k]

		# build + send, nearest first: new tiles, and loaded ones with a new scan or new PhysX buildings;
		# up to four a round while tiles close to the hero are missing
		built = 0
		held = sum(info.get("tris", 0) for info in loaded.values())
		budget_tris = a.max_tris * 1000
		for d, k in want:
			if built >= (4 if d < near_uu else 2): break
			info = loaded.get(k)
			if not info and d > near_uu:
				# outside --near only with room to spare (a tile's size from last time, else the average), and
				# not a tile the budget pushed out a moment ago: building it would push another one out, and the
				# two would take turns forever (2026-10-02: 1.8 million triangles rebuilt per 10 s standing still)
				est = last_tris.get(k, held / max(1, len(loaded)))
				if held + est > 0.85 * budget_tris or now - evicted.get(k, -1e9) < 20: continue
			if info and not info.get("stale"):
				# new PhysX buildings for a loaded tile: right away around the hero (a building that came in late
				# is otherwise wall-less in his collision: he got inside one, 2026-10-02), rarely farther out
				gain = shapes.count(k) - info["shapes"]
				if d < near_uu:
					if not (gain >= max(3, info["shapes"] // 20) and now - info["built"] > 8): continue
				elif not (d < 10000 and gain >= max(20, info["shapes"] // 5) and now - info["built"] > 45): continue
			if not info and not shapes.covers(k): continue
			if not scan_mtime(k) or k in scanning and not info: continue
			if heap is not None and heap < 64 and not info and d > near_uu: continue  # no room: only what is close
			try:
				awt, awh, tris, nh, water = build_tile(k, scan_path(k), shapes)
			except Exception as e:
				log("tile %s failed: %r" % (name_of(k), e))
				loaded[k] = {"shapes": shapes.count(k), "built": now}
				continue
			sm.tile(name_of(k), awt, awh)
			stats["rebuilt" if info else "built"] += 1
			stats["tris"] += tris
			stats["hints"] += nh
			held += tris - (info.get("tris", 0) if info else 0)
			loaded[k] = {"shapes": shapes.count(k), "built": now, "suspect": water > 0.25 and shapes.tall_count(k) >= 3, "tris": tris, "water": water}
			last_tris[k] = tris
			built += 1

		# unload far tiles; and while Spider-Man holds more triangles than --max-tris, the farthest ones
		# outside --near too (his Havok heap is fixed; there is no reading of it to go by)
		held = sum(info.get("tris", 0) for info in loaded.values())
		over = held > budget_tris
		for k in sorted(loaded, key=lambda kk: -np.hypot(*(center_of(kk) - pos[:2]))):
			d = np.hypot(*(center_of(k) - pos[:2]))
			if d > unload_uu or (over and held > 0.9 * budget_tris and d > near_uu):  # down to 90%: room for what comes next
				held -= loaded[k].get("tris", 0)
				sm.command("gotham stream unload %s" % name_of(k))
				if d <= unload_uu:
					evicted[k] = now
					stats["evicted"] += 1
				del loaded[k]
				stats["unloaded"] += 1
		stats["held"] = held

		# start: the hero's surroundings built in Spider-Man -> jump up (sky) / New York filter on
		around = all((c0[0] + di, c0[1] + dj) in loaded for di in (-1, 0, 1) for dj in (-1, 0, 1))
		if not jumped and around and gs["tilesQueued"] == 0 and not sm.queue and sm.ring.pending() == 0 and now - jump_sent > 6:
			sm.command("gotham stream jump")
			jump_sent = now
			log("start area built in Spider-Man: jumping up onto Gotham")
		if not jumped and gs["flags"] & proto.GUEST_ANCHORED:
			jumped = True
			log("Spider-Man stands on Gotham%s" % (" in the sky" if gs["flags"] & proto.GUEST_SKY else ""))
		if not jumped and gs["flags"] & proto.GUEST_JUMP_FAILED:
			log("the jump didn't hold: Gotham goes to Spider-Man's feet instead")
			sky = 0
			sm.command("gotham stream reset")
			sm.command("gotham stream begin %.1f %.1f %.1f 0" % tuple(origin))
			loaded.clear()
			jumped = True
		if jumped and not filter_on and around:
			sm.command("gotham filter on")
			filter_on = True
			log("New York filter ON")
		uncovered = filter_on and c0 not in loaded
		if uncovered and not warned:
			log("WARNING: the hero is in tile %s, which is not loaded yet (outran the streaming?)" % name_of(c0))
		warned = uncovered
		near_missing = any((c0[0] + di, c0[1] + dj) not in loaded for di in (-1, 0, 1) for dj in (-1, 0, 1))
		want_budget = a.budget * (2 if near_missing else 1)
		if want_budget != budget:
			budget = want_budget
			ak.command("scanbudget %g" % budget)

		ak.flush()
		if not sm.flush(): stats["ring full"] += 1
		if now - last_status > 10:
			last_status = now
			trace.flush()
			log("hero (%.0f %.0f %.0f) UU %.0f m/s | %d tiles loaded, %d scanning | Spider-Man holds %d (%d queued), heap %s MB free, rescues %d, "
			    "builds refused %d | zip: %d ledges, %s, %d launched, %d refused | %s" % (
			        pos[0], pos[1], pos[2], np.linalg.norm(vel) / 100, len(loaded), len(scanning), gs["tilesHeld"], gs["tilesQueued"],
			        heap if heap is not None else "?", gs["rescues"], gs["buildsRefused"], gs["zipEdges"],
			        "target" if gs["zipFlags"] & proto.ZIP_TARGET else "no target", gs["zipStarted"], gs["zipFailed"],
			        ", ".join("%s %d" % kv for kv in sorted(stats.items()))))
		time.sleep(0.05)
	sm.command("gotham filter off")
	sm.flush()


if __name__ == "__main__":
	main()
