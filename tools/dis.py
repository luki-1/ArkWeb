"""dis.py <exe> <rva> [<rva> ...]: disassemble the functions containing each RVA, with string,
RTTI-class and function-name hints on RIP-relative operands."""
import json, os, re, sys
from pe_tools import Image

img = Image(sys.argv[1])
names = {}
here = os.path.dirname(os.path.abspath(__file__))
rtti = os.path.join(here, "..", "recon", sys.argv[1].split("/")[-1].split("\\")[-1].split(".")[0][:2].lower() + "_rtti.json")
if os.path.exists(rtti):
	for cls, c in json.load(open(rtti)).items():
		for v in c["vtables"]:
			names[v["rva"]] = "vtbl " + cls
			for i, m in enumerate(v["methods"]):
				names.setdefault(m, "%s::v%d" % (cls, i))
natives = os.path.join(here, "..", "recon", "ak_natives.json")
if "batmanak" in sys.argv[1].lower() and os.path.exists(natives):
	for name, rva in json.load(open(natives)).items():
		names.setdefault(rva, name)


def hint(t):
	if t in names:
		return names[t]
	sec = img.section_of(t)
	if sec in (".rdata", ".data"):
		s = img.cstr(t, 120)
		if len(s) >= 4 and all(32 <= ord(ch) < 127 for ch in s):
			return repr(s)
		w = img.mem[t:t + 120]
		if len(w) > 8 and w[1] == 0 and w[3] == 0 and 32 <= w[0] < 127:
			return "L" + repr(w.decode("utf-16-le", "replace").split("\0")[0])
	return ""


for a in sys.argv[2:]:
	raw = None
	if ":" in a:  # "rva:len" disassembles len bytes from rva, ignoring .pdata (chained unwind splits functions)
		a, n = a.split(":")
		raw = int(n, 16)
	rva = int(a, 16)
	f = img.func_at(rva)
	print("==== %x  func %s" % (rva, "%x-%x" % f if f else "?"))
	for ins in (list(img._cs.disasm(img.mem[rva:rva + raw], rva)) if raw else img.disasm(rva)):
		h = ""
		m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
		if m:
			t = ins.address + ins.size + (1 if m.group(1) == "+" else -1) * int(m.group(2), 16)
			h = "; [%x] %s" % (t, hint(t))
		elif ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
			h = "; " + hint(int(ins.op_str, 16))
		print("%8x  %-7s %s %s" % (ins.address, ins.mnemonic, ins.op_str, h))
