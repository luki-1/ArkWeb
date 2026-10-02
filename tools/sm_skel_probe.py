"""sm_skel_probe.py: read-only look at the hero's skeleton and animated pose in a running Spider-Man.exe.

The hero entity is HeroLocal +0x8 (the guest logs HeroLocal). Its components are a hash map at entity +0x80
(16-byte {type descriptor, component} slots, u16 capacity at +0x88; exe+16799a0 looks them up). This lists
them by RTTI class, takes AnimControllerComponent (vtable exe+3ced0e8) and searches two pointer levels under
it (and under the transform record's +0xd8, which the animation update needs) for:
  - hkaSkeleton objects (vtable exe+525dfb0): joint names, parents, reference pose
  - arrays of hkQsTransform (48 bytes: translation, rotation quaternion, scale) and of 4x4 / 3x4 matrices
Writes logs/sm_skeleton.json for the retargeting work.

  python sm_skel_probe.py
"""
import json
import os
import struct
import sys

import numpy as np

from sm_proc import Proc

HERE = os.path.dirname(os.path.abspath(__file__))
RTTI = os.path.join(HERE, "..", "recon", "sm_rtti.json")
OUT = os.path.join(HERE, "..", "logs", "sm_skeleton.json")
ANIM_CTRL_VT = 0x3ced0e8
HKA_SKELETON_VT = 0x525dfb0

p = Proc()
exe = p.exe
vt_names = {}
for cls, c in json.load(open(RTTI)).items():
	for v in c["vtables"]:
		vt_names[v["rva"]] = cls


def cls_of(obj):
	vt = p.u64(obj)
	return vt_names.get(vt - exe) if vt and exe <= vt < exe + 0x10000000 else None


def u16(a):
	r = p.read(a, 2)
	return struct.unpack("<H", r)[0] if r else 0


def cstr(a, n=96):
	r = p.read(a, n)
	if not r: return None
	s = r.split(b"\0")[0]
	return s.decode("ascii", "replace") if s and all(32 <= ch < 127 for ch in s) else None


hl = p.hero_local()
if not hl: sys.exit("no HeroLocal in the guest log")
ent = p.u64(hl + 8)
xf = p.u64(ent)
print("HeroLocal %x  entity %x  transform %x" % (hl, ent, xf))
entries, cap = p.u64(ent + 0x80), u16(ent + 0x88)
comps = []
raw = p.read(entries, cap * 16) if entries and cap else b""
for i in range(0, len(raw), 16):
	key, val = struct.unpack_from("<QQ", raw, i)
	if val: comps.append((val, cls_of(val), key - exe))
print("%d components:" % len(comps))
for val, cls, key in comps:
	print("  %x %-48s type exe+%x" % (val, cls, key))
anim = [v for v, c, k in comps if p.u64(v) == exe + ANIM_CTRL_VT]
print("AnimControllerComponent:", ["%x" % a for a in anim])


def is_quat(q):
	n = float(np.dot(q, q))
	return 0.98 < n < 1.02


def scan_qs(a, n=400):
	"""hkQsTransform array at a: returns how many in a row look like one (translation w ~0, unit quat, scale ~1)."""
	r = p.read(a, 48 * n)
	if not r: return 0
	f = np.frombuffer(r, np.float32).reshape(-1, 12)
	k = 0
	for row in f:
		t, q, s = row[0:4], row[4:8], row[8:12]
		if not (np.all(np.isfinite(row)) and is_quat(q) and np.all(np.abs(s[:3] - 1) < 0.2) and np.all(np.abs(t[:3]) < 3000)): break
		k += 1
	return k


def scan_mat(a, n=400, stride=64):
	r = p.read(a, stride * n)
	if not r: return 0
	f = np.frombuffer(r, np.float32).reshape(-1, stride // 4)
	k = 0
	for row in f:
		m = row[:12].reshape(3, 4)[:, :3] if stride == 48 else row[:16].reshape(4, 4)[:3, :3]
		if not (np.all(np.isfinite(row)) and np.allclose(m @ m.T, np.eye(3), atol=0.05)): break
		k += 1
	return k


def hka_skeleton(s):
	name = cstr(p.u64(s + 0x10))
	par_p, par_n = p.u64(s + 0x18), struct.unpack("<i", p.read(s + 0x20, 4))[0]
	bone_p, bone_n = p.u64(s + 0x28), struct.unpack("<i", p.read(s + 0x30, 4))[0]
	ref_p, ref_n = p.u64(s + 0x38), struct.unpack("<i", p.read(s + 0x40, 4))[0]
	if not (0 < bone_n < 2000): return None
	parents = list(struct.unpack("<%dh" % par_n, p.read(par_p, 2 * par_n))) if 0 < par_n < 2000 else []
	braw = p.read(bone_p, 16 * bone_n) or b""
	bones = [cstr(struct.unpack_from("<Q", braw, 16 * i)[0]) for i in range(bone_n)]
	ref = np.frombuffer(p.read(ref_p, 48 * ref_n), np.float32).reshape(-1, 12).tolist() if 0 < ref_n < 2000 else []
	return {"addr": s, "name": name, "joints": bones, "parents": parents, "ref_pose": ref}


found = {"skeletons": [], "qs_arrays": [], "mat_arrays": []}
seen = set()


SPAN = (0x1000, 0x300, 0x100)  # bytes read at each depth (wider near the component)


def walk(base, depth, path):
	if depth > 2 or base in seen: return
	seen.add(base)
	r = p.read(base, SPAN[depth])
	if not r: return
	for off in range(0, len(r) - 8, 8):
		q = struct.unpack_from("<Q", r, off)[0]
		if not (0x10000 < q < 0x7FFFFFFF0000) or q & 7 or exe <= q < exe + 0x10000000: continue
		here = "%s+%x" % (path, off)
		if p.u64(q) == exe + HKA_SKELETON_VT:
			sk = hka_skeleton(q)
			if sk and q not in [x["addr"] for x in found["skeletons"]]:
				sk["path"] = here
				found["skeletons"].append(sk)
				print("  hkaSkeleton at %s -> %x '%s': %d joints" % (here, q, sk["name"], len(sk["joints"])))
			continue
		k = scan_qs(q)
		if k >= 20:
			found["qs_arrays"].append({"path": here, "addr": q, "count": k})
			print("  %d hkQsTransforms at %s -> %x" % (k, here, q))
			continue
		for st in (64, 48):
			k = scan_mat(q, stride=st)
			if k >= 20:
				found["mat_arrays"].append({"path": here, "addr": q, "count": k, "stride": st})
				print("  %d matrices (stride %d) at %s -> %x" % (k, st, here, q))
				break
		else:
			walk(q, depth + 1, here)


for a in anim:
	print("under AnimControllerComponent %x:" % a)
	walk(a, 0, "anim")
md = p.u64(xf + 0xd8)
print("under transform+0xd8 (%x):" % md)
if md: walk(md, 0, "xf.d8")
hero = p.floats(xf + 0x30, 3)
found["hero_pos"] = list(hero) if hero is not None else None
for arr in found["qs_arrays"]:
	f = np.frombuffer(p.read(arr["addr"], 48 * arr["count"]), np.float32).reshape(-1, 12)
	arr["sample"] = f[:arr["count"]].tolist()
for arr in found["mat_arrays"]:
	f = np.frombuffer(p.read(arr["addr"], arr["stride"] * arr["count"]), np.float32).reshape(arr["count"], -1)
	arr["sample"] = f.tolist()
json.dump(found, open(OUT, "w"))
print("written", OUT)
