"""Probe camera objects: list float triples near the hero (positions) and unit quaternions inside
the objects and one pointer level down, sampled twice to show what changes.
  python sm_camprobe.py <obj addr> [<obj addr> ...]   (or no args: find by vtable like sm_camfind)"""
import sys, time
import numpy as np
import pe_tools  # noqa
from sm_proc import Proc
import sm_camfind as cf

p = Proc()
hero = p.hero_matrix()[3, :3]
if len(sys.argv) > 1:
	objs = [(int(a, 16), "?") for a in sys.argv[1:]]
else:
	targets = np.array([p.exe + v for v in cf.VTABLES], np.uint64)
	objs = []
	for base, size in p.regions():
		for off in range(0, size, 1 << 24):
			b = p.read(base + off, min(1 << 24, size - off))
			if not b: continue
			q = np.frombuffer(b, np.uint64)
			for idx in np.nonzero(np.isin(q, targets))[0]:
				objs.append((base + off + int(idx) * 8, cf.VTABLES[int(q[idx]) - p.exe]))
print("hero", np.round(hero, 2), "objects", [(hex(a), c) for a, c in objs], flush=True)

def blocks():
	out = []
	for a, cls in objs:
		out.append((a, "%s %x" % (cls, a)))
		b = p.read(a, 0x1000)
		for k, ptr in enumerate(np.frombuffer(b, np.uint64)):
			ptr = int(ptr)
			if 0x10000000000 < ptr < 0x7FFFFFFF0000 and not ptr & 7: out.append((ptr, "%s %x [+%x]" % (cls, a, k * 8)))
	return out

def near(buf):
	f = np.frombuffer(buf[:len(buf) // 4 * 4], np.float32).astype(np.float64)
	hits = []
	for i in range(len(f) - 2):
		v = f[i:i + 3]
		if np.all(np.isfinite(v)):
			d = np.linalg.norm(v - hero)
			if 0.2 < d < 25: hits.append((i * 4, v.copy(), d))
	return hits

bl = blocks()
snap = {}
for addr, name in bl:
	b = p.read(addr, 0x1000 if "[" not in name else 0x600)
	if b: snap[(addr, name)] = b
print("move the camera now", flush=True)
time.sleep(4)
for (addr, name), b0 in snap.items():
	b1 = p.read(addr, len(b0))
	if not b1: continue
	h0 = near(b0)
	if not h0: continue
	f1 = np.frombuffer(b1[:len(b1) // 4 * 4], np.float32)
	for off, v, d in h0[:12]:
		v1 = f1[off // 4: off // 4 + 3]
		print("%-50s +%-5x %s  d=%.1f  ->  %s" % (name, off, np.round(v, 2), d, np.round(v1, 2)))
