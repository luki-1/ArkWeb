"""strref.py <exe> <regex>: functions referencing strings matching regex."""
import re, sys
from pe_tools import Image
img = Image(sys.argv[1])
pat = re.compile(sys.argv[2])
hits = [(r, s) for r, s, k in img.strings(4) if pat.search(s)]
refs = img.xrefs([r for r, s in hits])
for r, s in hits:
	fs = sorted({img.func_at(x)[0] for x in refs[r] if img.func_at(x)})
	print("%x %r -> %s" % (r, s[:90], " ".join("%x" % f for f in fs[:12])))
