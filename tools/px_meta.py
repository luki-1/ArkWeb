"""Recover PhysX 3.3 virtual-slot numbers from the exported *GeneratedInfo constructors: each
property name string is paired with accessor functions; an accessor that is `mov rax,[rcx];
jmp/call [rax+N]` (possibly after small arg shuffles) gives the slot N of that getter/setter."""
import re, sys
import pefile
from pe_tools import Image

PATH = "F:/SteamLibrary/steamapps/common/Batman Arkham Knight/Binaries/Win64/PhysX3_x64.dll"
img = Image(PATH)
pe = pefile.PE(PATH, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
exports = {e.name.decode(): e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}


def slot_of(fn, depth=0):
	"""First vtable slot the accessor calls through `this`: returns (slot, kind) or None."""
	vtreg = None
	for ins in img._cs.disasm(img.mem[fn:fn + 0x80], fn):
		m = re.match(r"(r\w+), qword ptr \[rcx\]$", ins.op_str)
		if ins.mnemonic == "mov" and m:
			vtreg = m.group(1)
		m = re.match(r"qword ptr \[(r\w+) \+ (0x[0-9a-f]+)\]$", ins.op_str)
		if ins.mnemonic in ("jmp", "call") and m and m.group(1) == vtreg:
			return int(m.group(2), 16) // 8
		if ins.mnemonic in ("jmp", "call") and m is None and depth < 1 and ins.op_str.startswith("0x"):
			r = slot_of(int(ins.op_str, 16), depth + 1)
			if r is not None:
				return r
		if ins.mnemonic == "ret":
			break
	return None


def dump(cls):
	ctor = exports.get("??0%sGeneratedInfo@physx@@QEAA@XZ" % cls)
	if ctor is None:
		print(cls, "no ctor export"); return
	name, out = None, []
	for ins in img.disasm(ctor, 0x3000):
		m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
		if not m or ins.mnemonic != "lea":
			continue
		t = ins.address + ins.size + (1 if m.group(1) == "+" else -1) * int(m.group(2), 16)
		if img.section_of(t) == ".rdata":
			s = img.cstr(t, 64)
			if re.fullmatch(r"[A-Z][A-Za-z0-9]{1,40}", s):
				name = s
		elif img.in_text(t) and name:
			sl = slot_of(t)
			out.append((name, sl, t))
	seen = {}
	for n, sl, t in out:
		seen.setdefault(n, []).append(sl)
	print("== %s" % cls)
	for n, sls in seen.items():
		print("   %-28s slots %s" % (n, sls))


for c in sys.argv[1:]:
	dump(c)
