"""xrefsat.py <exe> <rva> [context]: every instruction that references rva, with a few instructions around it
(raw disassembly, so it works where .pdata splits a function)."""
import sys

from pe_tools import Image

img = Image(sys.argv[1])
t = int(sys.argv[2], 16)
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 12
for r in sorted(img.xrefs([t])[t]):
	print("-- ref at %x" % r)
	for ins in img._cs.disasm(img.mem[r - 0x30:r + 0x60], r - 0x30):
		if abs(ins.address - r) <= ctx * 5:
			print("   %8x %s %-7s %s" % (ins.address, ">" if ins.address == r else " ", ins.mnemonic, ins.op_str))
