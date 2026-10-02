"""ue3_natives.py <exe> <out.json>: UE3 native-function lookup tables ({"UClassexecFunc", &UClass::execFunc})
-> {name: function RVA}. These are the closest thing to symbols an RTTI-less UE3 build has."""
import json, re, sys
from pe_tools import Image
img = Image(sys.argv[1])
names = {r: s for r, s, k in img.strings(6, (".rdata",)) if k == "a" and re.fullmatch(r"[AU][A-Za-z0-9_]+exec[A-Za-z0-9_]+", s)}
refs = img.qword_refs(list(names))
out = {}
for r, locs in refs.items():
	for loc in locs:
		fn = img.u64(loc + 8) - img.base
		if 0 <= fn < len(img.mem) and img.in_text(fn):
			out[names[r]] = fn
json.dump(out, open(sys.argv[2], "w"), indent=0, sort_keys=True)
print(len(names), "names,", len(out), "resolved")
