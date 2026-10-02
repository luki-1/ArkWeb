"""ak_skel_probe.py: read-only look at Batman's skeleton and animated pose in a running BatmanAK.exe.

UE3 (Rocksteady, pack 4; recon/PHASE0b.md and the SDK dump): GObjObjects is an inline UObject*[785000] at
exe+340cbe4; UObject Name (FName) +0x3C, Class +0x44; GNames TArray<FNameEntry*> at exe+3a208b8, the name at
entry +0xC (flags & 1: UTF-16, flags & 2: pointer to ANSI). Batman is the RPawnPlayerBm; Pawn.Mesh +0x38C ->
SkeletalMeshComponent: SkeletalMesh +0x280, SpaceBases +0x3F4 and LocalAtoms +0x404 (TArray<BoneAtom>, 32
bytes: quaternion, translation, scale). SkeletalMesh.RefSkeleton +0xDC is a TArray<FMeshBone> (its stride
and parent field are found here). Writes logs/ak_skeleton.json.

  python ak_skel_probe.py
"""
import json
import os
import struct
import sys

import numpy as np

from sm_proc import Proc

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "ak_skeleton.json")
GOBJECTS, NOBJECTS, GNAMES = 0x340cbe4, 785000, 0x3a208b8

p = Proc(exe_name="BatmanAK.exe")
exe = p.exe
names_data = p.u64(exe + GNAMES)
name_cache = {}


def i32(a):
	r = p.read(a, 4)
	return struct.unpack("<i", r)[0] if r else 0


def name(idx):
	if idx in name_cache: return name_cache[idx]
	e = p.u64(names_data + 8 * idx) if idx >= 0 else 0
	s = None
	if e:
		h = p.read(e, 0x10C)
		if h:
			flags = struct.unpack_from("<I", h, 0)[0] & 3
			if flags & 2:
				r = p.read(struct.unpack_from("<Q", h, 0xC)[0], 0x100) or b""
				s = r.split(b"\0")[0].decode("latin-1")
			elif flags & 1:
				s = h[0xC:].decode("utf-16-le", "replace").split("\0")[0]
			else:
				s = h[0xC:].split(b"\0")[0].decode("latin-1")
	name_cache[idx] = s
	return s


def fname(a):
	r = p.read(a, 8)
	if not r: return None
	idx, num = struct.unpack("<ii", r)
	n = name(idx)
	return n if not n or num == 0 else "%s_%d" % (n, num - 1)


# Batman: the RPawnPlayerBm among all objects
table = b"".join(p.read(exe + GOBJECTS + off, min(1 << 20, NOBJECTS * 8 - off)) or b"" for off in range(0, NOBJECTS * 8, 1 << 20))
ptrs = np.frombuffer(table, np.uint64)
cls_names = {}
pawns = []
for o in ptrs:
	o = int(o)
	if not o: continue
	h = p.read(o + 0x3C, 0x10)
	if not h: continue
	cls = struct.unpack_from("<Q", h, 8)[0]
	if cls not in cls_names: cls_names[cls] = fname(cls + 0x3C)
	if cls_names[cls] == "RPawnPlayerBm" and not (fname(o + 0x3C) or "").startswith("Default__"): pawns.append(o)
print("RPawnPlayerBm:", ["%x %s" % (o, fname(o + 0x3C)) for o in pawns])
if not pawns: sys.exit("Batman not found")
pawn = pawns[0]
loc = struct.unpack("<3f", p.read(pawn + 0xC4, 12))
mesh = p.u64(pawn + 0x38C)
skm = p.u64(mesh + 0x280)
print("pawn %x at %s UU, mesh component %x (%s), skeletal mesh %x (%s)" % (pawn, np.round(loc), mesh, fname(mesh + 0x3C), skm, fname(skm + 0x3C)))


def atoms(a):
	ptr, n = p.u64(a), i32(a + 8)
	if not ptr or not 0 < n < 2000: return []
	return np.frombuffer(p.read(ptr, 32 * n), np.float32).reshape(n, 8).tolist()


space, local = atoms(mesh + 0x3F4), atoms(mesh + 0x404)
ref_ptr, ref_n = p.u64(skm + 0xDC), i32(skm + 0xE4)
print("SpaceBases %d, LocalAtoms %d, RefSkeleton %d" % (len(space), len(local), ref_n))
# FMeshBone (Rocksteady, 0x30 bytes, found live 2026-10-02): orientation quaternion +0x00, position +0x10
# (xyz, pad), FName +0x20, parent index +0x28 (the root's is 0), -1 at +0x2C
stride, parent_off = 0x30, 0x28
raw = p.read(ref_ptr, stride * ref_n) or b""
bones = [fname(ref_ptr + stride * i + 0x20) for i in range(ref_n)]
parents = [struct.unpack_from("<i", raw, stride * i + parent_off)[0] for i in range(ref_n)]
ref = [list(struct.unpack_from("<4f", raw, stride * i)) + list(struct.unpack_from("<3f", raw, stride * i + 0x10)) for i in range(ref_n)]
for i, b in enumerate(bones[:12]):
	print("  %2d %-28s parent %s  ref q %s t %s" % (i, b, parents[i] if parents else "?", np.round(ref[i][:4], 3), np.round(ref[i][4:], 1)))
json.dump({"pawn": pawn, "location_uu": loc, "bones": bones, "parents": parents, "ref_local": ref, "space_bases": space, "local_atoms": local,
           "stride": stride, "parent_off": parent_off}, open(OUT, "w"))
print("written", OUT)
