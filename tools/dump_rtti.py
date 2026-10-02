import json, sys, time
from pe_tools import Image
t = time.time()
img = Image(sys.argv[1])
cls = img.rtti()
json.dump(cls, open(sys.argv[2], "w"))
print(len(cls), "classes", sum(len(c["vtables"]) for c in cls.values()), "vtables", round(time.time() - t), "s")
