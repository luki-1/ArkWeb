"""Keep Spider-Man's window-active flags set from outside the process (stand-in for the guest's
HoldWindowActive while an older DLL is running). [exe+7b11770]+0xa0 -> window; +0x18/+0x19 = 1.

  python sm_hold_active.py [seconds]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

import pe_tools  # noqa: F401  (idle priority)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]


def find_pid(name):
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


def main():
	secs = float(sys.argv[1]) if len(sys.argv) > 1 else 3600
	pid = find_pid("Spider-Man.exe")
	if not pid: sys.exit("Spider-Man.exe not running")
	h = k32.OpenProcess(0x0008 | 0x0010 | 0x0020 | 0x0400, False, pid)
	mods = (ctypes.c_void_p * 1)(); need = wt.DWORD()
	psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
	exe = mods[0]

	def rd(addr, n):
		b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
		if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), b, n, ctypes.byref(got)): return None
		return b.raw

	print("pid %d exe %#x" % (pid, exe), flush=True)
	end, writes, last = time.time() + secs, 0, 0
	while time.time() < end:
		r = rd(exe + 0x7b11770, 8)
		app = int.from_bytes(r, "little") if r else 0
		r = rd(app + 0xa0, 8) if app else None
		win = int.from_bytes(r, "little") if r else 0
		f = rd(win + 0x18, 2) if win else None
		if f is None:
			if not find_pid("Spider-Man.exe"): print("game gone"); return
		elif f != b"\x01\x01":
			k32.WriteProcessMemory(h, ctypes.c_void_p(win + 0x18), b"\x01\x01", 2, None)
			writes += 1
		if time.time() - last > 30:
			print("%s flags held, %d writes" % (time.strftime("%H:%M:%S"), writes), flush=True); last = time.time()
		time.sleep(0.05)


if __name__ == "__main__":
	main()
