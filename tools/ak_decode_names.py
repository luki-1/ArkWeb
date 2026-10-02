"""Decode the GNames dump (logs/gnames_ptrs.bin + gnames_pool_*.bin) into recon/ak_names.txt.

FNameEntry (Rocksteady UE3, pack 4): u32 index<<2 | flags, FNameEntry* hashNext (+4), then at +0xC
either the name inline (flags & 1: UTF-16, else ANSI) or, with flags & 2, a pointer to an ANSI name
(hardcoded names point into the exe)."""
import bisect, glob, os, struct, sys
from pe_tools import Image

LOGS = os.path.join(os.path.dirname(__file__), "..", "logs")
EXE_BASE = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x7FF68DA70000
img = Image("F:/SteamLibrary/steamapps/common/Batman Arkham Knight/Binaries/Win64/BatmanAK.exe")
ptrs = struct.unpack("<%dQ" % (os.path.getsize(os.path.join(LOGS, "gnames_ptrs.bin")) // 8), open(os.path.join(LOGS, "gnames_ptrs.bin"), "rb").read())
pools = sorted((int(os.path.basename(p)[12:-4], 16), open(p, "rb").read()) for p in glob.glob(os.path.join(LOGS, "gnames_pool_*.bin")))
starts = [s for s, _ in pools]

def mem(addr, n):
	if EXE_BASE <= addr < EXE_BASE + len(img.mem):
		return img.mem[addr - EXE_BASE:addr - EXE_BASE + n]
	i = bisect.bisect_right(starts, addr) - 1
	if i >= 0 and addr - starts[i] < len(pools[i][1]):
		return pools[i][1][addr - starts[i]:addr - starts[i] + n]
	return None

out, bad = [], 0
for i, p in enumerate(ptrs):
	if not p:
		continue
	h = mem(p, 0x14)
	if not h or len(h) < 0x14:
		bad += 1; continue
	idx, = struct.unpack_from("<I", h, 0)
	flags = idx & 3
	if idx >> 2 != i:
		bad += 1; continue
	src = struct.unpack_from("<Q", h, 0xC)[0] if flags & 2 else p + 0xC
	raw = mem(src, 2048)
	if raw is None:
		bad += 1; out.append((i, "<?%x>" % src)); continue
	if flags & 1 and not flags & 2:
		s = raw.decode("utf-16-le", "replace").split("\0")[0]
	else:
		s = raw.split(b"\0")[0].decode("latin-1")
	out.append((i, s))
with open(os.path.join(os.path.dirname(__file__), "..", "recon", "ak_names.txt"), "w", encoding="utf-8") as f:
	for i, s in out:
		f.write("%d\t%s\n" % (i, s))
print(len(out), "names,", bad, "bad")
