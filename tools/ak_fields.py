"""Load logs/ak_objects.txt + logs/ak_fields.bin (from the AK recon DLL's objlist)."""
import os, struct
LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "recon_ak2")

def load():
	objs = {}
	with open(os.path.join(LOGS, "ak_objects.txt"), encoding="latin-1") as f:
		for line in f:
			i, p, cls, outer, name = line.rstrip("\n").split("\t")
			objs[int(i)] = {"ptr": int(p, 16), "cls": cls, "outer": int(outer), "full": name}
	byptr = {o["ptr"]: i for i, o in objs.items()}
	raw = {}
	with open(os.path.join(LOGS, "ak_fields.bin"), "rb") as f:
		while True:
			h = f.read(8)
			if len(h) < 8: break
			i, n = struct.unpack("<II", h)
			raw[i] = f.read(n)
	return objs, byptr, raw
