"""Print a UEnum's value names from the running Arkham Knight (ReadProcessMemory only).
  python ak_enum.py ECollisionFilter"""
import struct, sys
import numpy as np
import pe_tools  # noqa
from sm_proc import Proc

GNAMES, GOBJ, GOBJ_NUM = 0x3a208b8, 0x340cbe4, 0x3a09f28
p = Proc(exe_name="BatmanAK.exe")
names_data, names_num = struct.unpack("<QI", p.read(p.exe + GNAMES, 12))

def name(i):
	e = p.u64(names_data + 8 * i)
	if not e: return None
	flags = struct.unpack("<I", p.read(e, 4))[0]
	raw = p.read(e + 0xC, 128)
	if flags & 1: return raw.decode("utf-16-le", "replace").split("\0")[0]
	return raw.split(b"\0")[0].decode("latin-1")

want = sys.argv[1]
target = next(i for i in range(names_num) if name(i) == want) if False else None
# find the name index by scanning (names are few hundred thousand; read the pointer table once)
ptrs = np.frombuffer(p.read(names_data, 8 * names_num), np.uint64)
for i, e in enumerate(ptrs):
	if not e: continue
	raw = p.read(int(e) + 0xC, len(want) * 2 + 2)
	if raw and (raw[:len(want)] == want.encode() and raw[len(want)] == 0):
		target = i; break
print("name index", target)
num = struct.unpack("<I", p.read(p.exe + GOBJ_NUM, 4))[0]
objs = np.frombuffer(p.read(p.exe + GOBJ, 8 * num), np.uint64)
for o in objs:
	o = int(o)
	if not o: continue
	r = p.read(o + 0x3C, 4)
	if r and struct.unpack("<i", r)[0] == target:
		blob = p.read(o, 0x80)
		print("object %x class name %s" % (o, name(struct.unpack("<i", p.read(p.u64(o + 0x44) + 0x3C, 4))[0])))
		for off in range(0x48, 0x78, 4):
			d, n = struct.unpack("<QI", blob[off:off + 12])
			if 0 < n < 64 and d > 0x10000:
				vals = p.read(d, 8 * n)
				nm = [name(struct.unpack("<i", vals[8 * k:8 * k + 4])[0]) for k in range(n)]
				if all(nm): print("  TArray<FName> at +%x:" % off, list(enumerate(nm)))
