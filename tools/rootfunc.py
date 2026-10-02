"""rootfunc.py <exe> <rva>...: the primary function of each RVA (follows chained unwind info, so a
chunk split off by the compiler maps back to its parent), with the strings that function references."""
import re, struct, sys
from pe_tools import Image

img = Image(sys.argv[1])


def root(rva):
	f = img.func_at(rva)
	if not f:
		return None
	begin = f[0]
	for _ in range(16):
		img._load_pdata()
		# find the RUNTIME_FUNCTION for begin
		ent = None
		for b, e, u in getattr(img, "_pdata_full", []):
			if b == begin:
				ent = (b, e, u)
				break
		if ent is None:
			break
		u = ent[2]
		flags = img.mem[u] >> 3
		if not flags & 4:
			break
		n = img.mem[u + 2]
		off = u + 4 + 2 * (n + (n & 1))
		begin = struct.unpack_from("<I", img.mem, off)[0]
	return begin


def strings_in(fn):
	out = set()
	for ins in img.disasm(fn):
		m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
		if m:
			t = ins.address + ins.size + (1 if m.group(1) == "+" else -1) * int(m.group(2), 16)
			if img.section_of(t) == ".rdata":
				s = img.cstr(t, 100)
				if len(s) >= 6 and all(32 <= ord(c) < 127 for c in s):
					out.add(s)
	return out


if __name__ == "__main__":
	# build a full pdata table with unwind rvas
	sec = img.sections.get(".pdata")
	va, vs = sec
	full = []
	for i in range(0, vs - 11, 12):
		b, e, u = struct.unpack_from("<III", img.mem, va + i)
		if b:
			full.append((b, e, u))
	img._pdata_full = full
	for a in sys.argv[2:]:
		r = root(int(a, 16))
		ss = strings_in(r) if r else set()
		print("%s -> root %x : %s" % (a, r or 0, " | ".join(sorted(ss))[:400]))
