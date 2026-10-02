"""Light static-analysis helpers for 64-bit game executables (no Ghidra needed).

Load once with `Image(path)`, then:
  img.rtti()            -> {class name: {"vtables": [...], "bases": [...]}} from MSVC x64 RTTI
  img.strings()         -> [(rva, text, "a"|"w")] ASCII and UTF-16 strings in data sections
  img.xrefs(rva)        -> RVAs of instructions that reference `rva` RIP-relatively (lea/mov/call/jmp)
  img.func_at(rva)      -> (begin, end) from the .pdata exception table
  img.disasm(rva)       -> list of capstone instructions for the function containing rva
  img.callees(rva)      -> direct call targets in that function

Everything is RVA based. Importing this module drops the process to idle priority on one core,
so the analysis never loads the machine.
"""
import bisect
import ctypes
import re
import struct

_k32 = ctypes.windll.kernel32
_k32.SetPriorityClass(_k32.GetCurrentProcess(), 0x40)  # IDLE_PRIORITY_CLASS
_k32.SetProcessAffinityMask(_k32.GetCurrentProcess(), 1)

import numpy as np
import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs


class Image:
	def __init__(self, path):
		pe = pefile.PE(path, fast_load=True)
		self.base = pe.OPTIONAL_HEADER.ImageBase
		size = pe.OPTIONAL_HEADER.SizeOfImage
		raw = open(path, "rb").read()
		self.mem = bytearray(size)
		self.sections = {}
		for s in pe.sections:
			name = s.Name.rstrip(b"\0").decode(errors="replace")
			data = raw[s.PointerToRawData:s.PointerToRawData + min(s.SizeOfRawData, s.Misc_VirtualSize)]
			self.mem[s.VirtualAddress:s.VirtualAddress + len(data)] = data
			self.sections[name] = (s.VirtualAddress, s.Misc_VirtualSize)
		self.mem = bytes(self.mem)
		self._pdata = None
		self._cs = Cs(CS_ARCH_X86, CS_MODE_64)
		self._cs.detail = False

	# ---- basics ---------------------------------------------------------------------------
	def u32(self, rva):
		return struct.unpack_from("<I", self.mem, rva)[0]

	def i32(self, rva):
		return struct.unpack_from("<i", self.mem, rva)[0]

	def u64(self, rva):
		return struct.unpack_from("<Q", self.mem, rva)[0]

	def section_of(self, rva):
		for name, (va, vs) in self.sections.items():
			if va <= rva < va + vs:
				return name
		return None

	def in_text(self, rva):
		va, vs = self.sections[".text"]
		return va <= rva < va + vs

	def cstr(self, rva, limit=512):
		end = self.mem.find(b"\0", rva, rva + limit)
		return self.mem[rva:end].decode(errors="replace")

	# ---- RTTI -----------------------------------------------------------------------------
	def rtti(self):
		"""Classes with their vtables (COL offset = subobject offset) and base classes."""
		td_name = {}
		for m in re.finditer(rb"\.\?A[VU][\x20-\x7e]{1,400}?@@\0", self.mem):
			td_name[m.start() - 16] = m.group()[:-1].decode()

		classes = {}
		rd_va, rd_vs = self.sections[".rdata"]
		rd = np.frombuffer(self.mem, dtype=np.uint32, count=rd_vs // 4, offset=rd_va)
		# CompleteObjectLocator: sig=1, off, cdOff, pTD, pCHD, pSelf (all RVAs); pSelf == own RVA.
		cand = np.nonzero(rd == 1)[0]
		cols = {}
		for i in cand:
			rva = rd_va + int(i) * 4
			if rva + 24 > rd_va + rd_vs:
				continue
			sig, off, cd, ptd, pchd, pself = struct.unpack_from("<IIIIII", self.mem, rva)
			if pself == rva and ptd in td_name:
				cols[rva] = (off, ptd, pchd)

		# vtables: the qword before the first slot points at the COL.
		col_va = {self.base + c: c for c in cols}
		q = np.frombuffer(self.mem, dtype=np.uint64, count=rd_vs // 8, offset=rd_va)
		hits = np.nonzero(np.isin(q, np.array(list(col_va), dtype=np.uint64)))[0]
		for i in hits:
			col = col_va[int(q[i])]
			off, ptd, pchd = cols[col]
			vt = rd_va + (int(i) + 1) * 8
			methods = []
			while vt + len(methods) * 8 < rd_va + rd_vs:
				p = self.u64(vt + len(methods) * 8) - self.base
				if not (0 <= p < len(self.mem)) or not self.in_text(p):
					break
				methods.append(p)
			name = demangle(td_name[ptd])
			c = classes.setdefault(name, {"vtables": [], "bases": None})
			c["vtables"].append({"rva": vt, "offset": off, "methods": methods})
			if c["bases"] is None:
				c["bases"] = self._bases(pchd, td_name)
		for name, c in classes.items():
			c["vtables"].sort(key=lambda v: v["offset"])
		return classes

	def _bases(self, pchd, td_name):
		try:
			sig, attr, n, pbca = struct.unpack_from("<IIII", self.mem, pchd)
			out = []
			for k in range(1, min(n, 64)):  # entry 0 is the class itself
				bcd = self.u32(pbca + 4 * k)
				ptd, ncont, mdisp = struct.unpack_from("<IIi", self.mem, bcd)
				out.append({"name": demangle(td_name.get(ptd, "?")), "mdisp": mdisp})
			return out
		except struct.error:
			return []

	# ---- strings --------------------------------------------------------------------------
	def strings(self, min_len=5, sections=(".rdata", ".data")):
		out = []
		for sec in sections:
			if sec not in self.sections:
				continue
			va, vs = self.sections[sec]
			blob = self.mem[va:va + vs]
			for m in re.finditer(rb"[\x20-\x7e]{%d,}\0" % min_len, blob):
				out.append((va + m.start(), m.group()[:-1].decode(), "a"))
			for m in re.finditer(rb"(?:[\x20-\x7e]\0){%d,}\0\0" % min_len, blob):
				if m.start() % 2 == 0:
					out.append((va + m.start(), m.group()[:-2].decode("utf-16-le"), "w"))
		return out

	# ---- cross references -----------------------------------------------------------------
	def _disp_view(self):
		if not hasattr(self, "_text"):
			va, vs = self.sections[".text"]
			self._text_va = va
			self._text = np.frombuffer(self.mem, dtype=np.uint8, count=vs, offset=va)
		return self._text_va, self._text

	def xrefs(self, targets, chunk=1 << 24):
		"""RIP-relative references to any of `targets` (int or iterable of RVAs).

		Assumes the disp32 is the instruction's last field (true for lea/mov r,[rip]/call/jmp);
		returns {target: [rva of the disp32 field]}. Callers can disassemble around it.
		"""
		if isinstance(targets, int):
			targets = [targets]
		tset = np.array(sorted(set(targets)), dtype=np.int64)
		va, t = self._disp_view()
		out = {int(x): [] for x in tset}
		n = len(t)
		for s in range(0, n - 4, chunk):
			e = min(n - 4, s + chunk)
			b = t[s:e + 4].astype(np.int64)
			d = b[:-4] | (b[1:-3] << 8) | (b[2:-2] << 16) | (b[3:-1] << 24)
			d = np.where(d >= 1 << 31, d - (1 << 32), d)
			tgt = d + (va + s + 4) + np.arange(e - s, dtype=np.int64)
			idx = np.nonzero(np.isin(tgt, tset))[0]
			for i in idx:
				out[int(tgt[i])].append(va + s + int(i))
		return out

	def qword_refs(self, targets):
		"""Absolute 64-bit pointers (base + rva) to `targets` in .rdata/.data (vtables, tables)."""
		vals = np.array([self.base + t for t in targets], dtype=np.uint64)
		out = {t: [] for t in targets}
		for sec in (".rdata", ".data"):
			va, vs = self.sections[sec]
			q = np.frombuffer(self.mem, dtype=np.uint64, count=vs // 8, offset=va)
			for i in np.nonzero(np.isin(q, vals))[0]:
				out[int(q[i]) - self.base].append(va + int(i) * 8)
		return out

	# ---- functions ------------------------------------------------------------------------
	def _load_pdata(self):
		if self._pdata is None:
			va, vs = self.sections[".pdata"]
			a = np.frombuffer(self.mem, dtype=np.uint32, count=vs // 4, offset=va).reshape(-1, 3)
			a = a[a[:, 0] != 0]
			a = a[np.argsort(a[:, 0])]
			self._pdata = (a[:, 0].tolist(), a[:, 1].tolist())
		return self._pdata

	def func_at(self, rva):
		begins, ends = self._load_pdata()
		i = bisect.bisect_right(begins, rva) - 1
		if i >= 0 and begins[i] <= rva < ends[i]:
			return begins[i], ends[i]
		return None

	def disasm(self, rva, max_bytes=0x4000):
		f = self.func_at(rva)
		b, e = f if f else (rva, rva + 0x200)
		e = min(e, b + max_bytes)
		return list(self._cs.disasm(self.mem[b:e], b))

	def callees(self, rva):
		out = []
		for ins in self.disasm(rva):
			if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
				out.append(int(ins.op_str, 16))
		return out


def demangle(td):
	"""'.?AVFoo@Bar@@' -> 'Bar::Foo' (good enough for searching; templates left as-is)."""
	s = td[4:-2] if td.endswith("@@") else td[4:]
	if "?$" in s:
		return s
	return "::".join(reversed([p for p in s.split("@") if p]))
