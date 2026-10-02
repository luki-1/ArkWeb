"""Minimal minidump reader: the faulting module + RVA, registers, and a heuristic stack scan (qwords on
the faulting thread's stack that point into modules = likely return addresses).

  python minidump.py <file.mdmp> [stack qwords to scan, default 400]
"""
import struct
import sys


def main():
	path = sys.argv[1]
	depth = int(sys.argv[2]) if len(sys.argv) > 2 else 400
	b = open(path, "rb").read()
	assert b[:4] == b"MDMP"
	n, dir_rva = struct.unpack_from("<II", b, 8)
	streams = {}
	for i in range(n):
		t, size, rva = struct.unpack_from("<III", b, dir_rva + 12 * i)
		streams.setdefault(t, (size, rva))
	# modules
	mods = []
	cnt, = struct.unpack_from("<I", b, streams[4][1])
	for i in range(cnt):
		o = streams[4][1] + 4 + 108 * i
		base, size = struct.unpack_from("<QI", b, o)
		name_rva, = struct.unpack_from("<I", b, o + 20)
		ln, = struct.unpack_from("<I", b, name_rva)
		name = b[name_rva + 4:name_rva + 4 + ln].decode("utf-16-le")
		mods.append((base, size, name.split("\\")[-1]))

	def where(a):
		for base, size, name in mods:
			if base <= a < base + size: return "%s+%x" % (name, a - base)
		return None

	# memory ranges (for reading the stack)
	mem = []
	if 9 in streams:  # Memory64List
		o = streams[9][1]
		nr, base_rva = struct.unpack_from("<QQ", b, o)
		off = base_rva
		for i in range(nr):
			start, size = struct.unpack_from("<QQ", b, o + 16 + 16 * i)
			mem.append((start, size, off))
			off += size
	if 5 in streams:  # MemoryList
		o = streams[5][1]
		nr, = struct.unpack_from("<I", b, o)
		for i in range(nr):
			start, size, rva = struct.unpack_from("<QII", b, o + 4 + 16 * i)
			mem.append((start, size, rva))

	def read(a, n):
		for start, size, rva in mem:
			if start <= a and a + n <= start + size: return b[rva + a - start:rva + a - start + n]
		return None

	# exception
	o = streams[6][1]
	tid, = struct.unpack_from("<I", b, o)
	code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", b, o + 8)
	info = struct.unpack_from("<15Q", b, o + 8 + 32)
	ctx_size, ctx_rva = struct.unpack_from("<II", b, o + 8 + 32 + 120)
	print("exception %08x at %x = %s, thread %d" % (code, addr, where(addr), tid))
	if code == 0xC0000005: print("  access violation: %s address %x" % ({0: "read", 1: "write", 8: "execute"}.get(info[0], "?"), info[1]))
	names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
	regs = struct.unpack_from("<16Q", b, ctx_rva + 0x78)
	rip, = struct.unpack_from("<Q", b, ctx_rva + 0xF8)
	print("  rip %x %s" % (rip, where(rip) or ""))
	for k, v in zip(names, regs):
		w = where(v)
		print("  %-3s %016x %s" % (k, v, w or ""))
	rsp = regs[4]
	print("stack scan from rsp (module pointers):")
	shown = 0
	for i in range(depth):
		q = read(rsp + 8 * i, 8)
		if q is None: break
		v, = struct.unpack("<Q", q)
		w = where(v)
		if w and not w.lower().startswith(("ntdll", "kernelbase")) or (w and shown < 3):
			print("  [rsp+%03x] %s" % (8 * i, w))
			shown += 1
	print("modules near the fault:")
	for base, size, name in sorted(mods):
		if name.lower() in ("spider-man.exe", "winmm.dll") or base <= addr < base + size: print("  %x-%x %s" % (base, base + size, name))


if __name__ == "__main__":
	main()
