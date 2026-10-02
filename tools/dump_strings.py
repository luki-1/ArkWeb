import sys
from pe_tools import Image
img = Image(sys.argv[1])
with open(sys.argv[2], "w", encoding="utf-8") as f:
	for rva, s, k in img.strings(4):
		f.write("%08x %s %s\n" % (rva, k, s))
