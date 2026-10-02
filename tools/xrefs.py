"""xrefs.py <exe> <rva> [<rva> ...]: functions that reference each RVA (RIP-relative lea/mov/call/jmp)."""
import sys

from pe_tools import Image

img = Image(sys.argv[1])
targets = [int(a, 16) for a in sys.argv[2:]]
refs = img.xrefs(targets)
for t in targets:
	fs = sorted({img.func_at(x)[0] for x in refs[t] if img.func_at(x)})
	print("%x: %d refs in %d functions: %s" % (t, len(refs[t]), len(fs), " ".join("%x" % f for f in fs[:30])))
