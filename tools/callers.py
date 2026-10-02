"""callers.py <exe> <rva> ...: direct call/jmp sites (E8/E9 rel32) of each function, with the
containing function."""
import sys
import numpy as np
from pe_tools import Image
img = Image(sys.argv[1])
va, t = img._disp_view()
targets = [int(a, 16) for a in sys.argv[2:]]
refs = img.xrefs(targets)
for tg in targets:
	sites = [r for r in refs[tg] if img.mem[r - 1] in (0xE8, 0xE9)]
	print("%x: %d call sites" % (tg, len(sites)))
	for r in sites[:40]:
		f = img.func_at(r)
		print("   %x %s in %s" % (r - 1, "call" if img.mem[r - 1] == 0xE8 else "jmp ", "%x" % f[0] if f else "?"))
