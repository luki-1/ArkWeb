"""Read-only access to the running Spider-Man.exe from outside (ReadProcessMemory), for tools that
must not need a new guest DLL: find the process, the exe base, the hero transform, heap regions."""
import ctypes
import ctypes.wintypes as wt
import re
import struct

import numpy as np

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]

LOG = r"H:\SteamLibrary\steamapps\common\Saints Row the Third\ArkWeb\logs\sm_guest.log"


class MBI(ctypes.Structure):
	_fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", wt.DWORD),
	            ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


def find_pid(name="Spider-Man.exe"):
	arr = (wt.DWORD * 4096)(); got = wt.DWORD()
	psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(got))
	for pid in arr[:got.value // 4]:
		h = k32.OpenProcess(0x1000, False, pid)
		if not h: continue
		buf = ctypes.create_unicode_buffer(260); n = wt.DWORD(260)
		ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
		k32.CloseHandle(h)
		if ok and buf.value.lower().endswith(name.lower()): return pid
	return None


class Proc:
	def __init__(self, write=False, exe_name="Spider-Man.exe"):
		self.pid = find_pid(exe_name)
		if not self.pid: raise SystemExit(exe_name + " not running")
		access = 0x0010 | 0x0400 | (0x0008 | 0x0020 if write else 0)
		self.h = k32.OpenProcess(access, False, self.pid)
		mods = (ctypes.c_void_p * 1)(); need = wt.DWORD()
		psapi.EnumProcessModulesEx(self.h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
		self.exe = mods[0]

	def read(self, addr, n):
		b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
		if not addr or not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), b, n, ctypes.byref(got)): return None
		return b.raw

	def u64(self, addr):
		r = self.read(addr, 8)
		return struct.unpack("<Q", r)[0] if r else 0

	def floats(self, addr, n):
		r = self.read(addr, 4 * n)
		return np.frombuffer(r, np.float32) if r else None

	def regions(self, max_size=1 << 30):
		"""Committed private read-write regions (no guard / no-cache / write-combine)."""
		addr, mbi = 0x10000, MBI()
		while addr < 0x7FFFFFFF0000:
			if not k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)): break
			if mbi.State == 0x1000 and mbi.Type == 0x20000 and mbi.Protect == 0x04 and mbi.RegionSize <= max_size:
				yield mbi.BaseAddress, mbi.RegionSize
			addr = mbi.BaseAddress + mbi.RegionSize

	def hero_local(self):
		"""HeroLocal pointer from the guest log of this run."""
		last = None
		for line in open(LOG, errors="replace"):
			m = re.search(r"HeroLocal ([0-9A-Fa-f]{8,16})", line)
			if m: last = int(m.group(1), 16)
		return last

	def _matrix_of(self, hl):
		rec = self.u64(hl + 8) if hl else 0
		xf = self.u64(rec) if rec else 0
		m = self.floats(xf, 16) if xf else None
		if m is None or not np.all(np.isfinite(m)) or abs(m[15] - 1) > 1e-3 or abs(np.linalg.norm(m[4:7]) - 1) > 0.01: return None
		return m.reshape(4, 4)

	def find_hero_locals(self):
		"""Every Hero::HeroLocal object in the heap (by vtable), e.g. after a checkpoint reload."""
		vt = np.uint64(self.exe + 0x38a93c8)
		out = []
		for base, size in self.regions():
			for off in range(0, size, 1 << 24):
				b = self.read(base + off, min(1 << 24, size - off))
				if b:
					out += [base + off + int(i) * 8 for i in np.nonzero(np.frombuffer(b, np.uint64) == vt)[0]]
		return out

	def hero_matrix(self, hero_local=None):
		if hero_local: return self._matrix_of(hero_local)
		hl = getattr(self, "_hl", None) or self.hero_local()
		m = self._matrix_of(hl)
		if m is None or not np.any(m[3, :3]):
			for cand in self.find_hero_locals():
				m = self._matrix_of(cand)
				if m is not None and np.any(m[3, :3]):
					hl = cand
					break
		self._hl = hl
		return m
