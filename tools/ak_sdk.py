"""Generate a readable Arkham Knight SDK (recon/ak_sdk.txt) from the recon DLL's objlist dump.

Layouts (Rocksteady UE3, pack 4; see recon/PHASE0b.md):
  UField:    Next +0x54
  UStruct:   SuperField +0x5C, Children +0x64, PropertySize u16 +0x78
  UProperty: ArrayDim u32 +0x5C, PropertyFlags u64 +0x60, ElementSize u16 +0x68, Offset u16 +0x6A,
             type data +0x94 (Struct / PropertyClass / Inner / Enum / bool BitMask), ClassProperty
             MetaClass +0x9C
  UFunction: FunctionFlags u32 +0xA4, native exec pointer +0xBC
"""
import json, os, struct
from ak_fields import load

HERE = os.path.dirname(os.path.abspath(__file__))
objs, byptr, raw = load()
EXE_BASE = None  # set from a native function pointer below


def q(b, o):
	return struct.unpack_from("<Q", b, o)[0] if b and o + 8 <= len(b) else 0


def u16(b, o):
	return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
	return struct.unpack_from("<I", b, o)[0]


def idx(p):
	return byptr.get(p)


def short(i):
	return objs[i]["full"].split(".")[-1] if i is not None else "?"


def children(i):
	out, p, seen = [], q(raw.get(i), 0x64), set()
	while p and p not in seen:
		seen.add(p)
		c = idx(p)
		if c is None or c not in raw:
			break
		out.append(c)
		p = q(raw[c], 0x54)
	return out


def ptype(i):
	cls, b = objs[i]["cls"], raw[i]
	t = q(b, 0x94)
	if cls == "StructProperty":
		return "struct " + short(idx(t))
	if cls == "ObjectProperty" or cls == "ComponentProperty":
		return short(idx(t)) + "*"
	if cls == "ClassProperty":
		return "class<%s>" % short(idx(q(b, 0x9C)))
	if cls == "ArrayProperty":
		inner = idx(t)
		return "TArray<%s>" % (ptype(inner) if inner in raw else "?")
	if cls == "ByteProperty":
		return "byte" + (" /*enum %s*/" % short(idx(t)) if t else "")
	if cls == "BoolProperty":
		return "bool /*mask 0x%x*/" % u32(b, 0x94)
	if cls == "InterfaceProperty":
		return "interface " + short(idx(t))
	return {"IntProperty": "int", "FloatProperty": "float", "NameProperty": "FName", "StrProperty": "FString",
	        "DelegateProperty": "delegate", "MapProperty": "TMap"}.get(cls, cls)


lines, natives = [], {}
structs = sorted((i for i, o in objs.items() if o["cls"] in ("Class", "ScriptStruct") and i in raw), key=lambda i: objs[i]["full"])
for s in structs:
	b = raw[s]
	sup = idx(q(b, 0x5C))
	lines.append("%s %s : %s   // size 0x%x" % ("class" if objs[s]["cls"] == "Class" else "struct", objs[s]["full"],
	                                            objs[sup]["full"] if sup is not None else "-", u16(b, 0x78)))
	kids = children(s)
	props = [k for k in kids if objs[k]["cls"].endswith("Property")]
	for p in sorted(props, key=lambda k: (u16(raw[k], 0x6A), u32(raw[k], 0x94))):
		pb = raw[p]
		dim = u32(pb, 0x5C)
		lines.append("    +0x%04x %-40s %s%s   // size 0x%x flags 0x%x" % (u16(pb, 0x6A), ptype(p), short(p),
		             "[%d]" % dim if dim > 1 else "", u16(pb, 0x68), q(pb, 0x60)))
	for f in kids:
		if objs[f]["cls"] != "Function":
			continue
		fb = raw[f]
		params = [k for k in children(f) if objs[k]["cls"].endswith("Property")]
		ps = ", ".join("%s %s@0x%x/%x" % (ptype(k), short(k), u16(raw[k], 0x6A), q(raw[k], 0x60)) for k in params)
		fn = q(fb, 0xBC)
		natives[objs[f]["full"]] = fn
		lines.append("    fn %s(%s)   // flags 0x%x native %x" % (short(f), ps, u32(fb, 0xA4), fn))
	lines.append("")

out = os.path.join(HERE, "..", "recon", "ak_sdk.txt")
open(out, "w", encoding="utf-8").write("\n".join(lines))
print("wrote", out, len(structs), "classes/structs,", len(lines), "lines")
