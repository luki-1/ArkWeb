"""Python side of protocol/arkweb_protocol.h (for the fake host/guest stand-ins).

`python arkweb_proto.py --check` compares these offsets with `bin/link_test.exe --layout`.
"""
import ctypes
import json
import mmap
import os
import struct
import subprocess
import sys
import time

MAGIC, VERSION = 0x574B5241, 5
MAPPING = "Local\\ArkWeb_v1"
OFF_HEADER, OFF_HOST, OFF_PAD, OFF_GUEST = 0x0, 0x100, 0x200, 0x300
OFF_CAPTURE = 0x800
OFF_HOST_RING, HOST_RING_BYTES = 0x11000, 0xF000
OFF_COLLISION_RING, COLLISION_RING_BYTES = 0x20000, 32 << 20
MAPPING_BYTES = OFF_COLLISION_RING + COLLISION_RING_BYTES
HEARTBEAT_TIMEOUT_MS = 1500

HOST_IN_GAME, HOST_MENU_OPEN, HOST_LOADING, HOST_DRIVE_PUPPET = 1, 2, 4, 8
PAD_ACTIVE, PAD_CAMERA, PAD_ANALOG = 1, 2, 4
GUEST_IN_WORLD, GUEST_FOCUS_FAKED, GUEST_ANCHORED, GUEST_SKY, GUEST_JUMP_FAILED = 1, 2, 4, 8, 16
REC_PAD, REC_COMMAND, REC_TILE = 0, 1, 2
RING_HEAD, RING_TAIL, RING_DATA = 0x00, 0x40, 0x80

# name: (offset, struct format) relative to the slot
HEADER = {"magic": (0, "I"), "version": (4, "I"), "hostPid": (8, "I"), "guestPid": (12, "I"),
          "hostHeartbeatMs": (16, "Q"), "guestHeartbeatMs": (24, "Q")}
HOST = {"seq": (0, "I"), "flags": (4, "I"), "frame": (8, "Q"), "deltaTime": (0x10, "f"), "teleportSeq": (0x14, "I"),
        "teleportPos": (0x18, "3d"), "teleportYaw": (0x30, "f"), "worldId": (0x34, "I"), "puppetPos": (0x38, "3d"),
        "puppetYaw": (0x50, "f"), "collisionEpoch": (0x54, "I")}
PAD = {"seq": (0, "I"), "flags": (4, "I"), "packet": (8, "I"), "buttons": (0xC, "H"), "leftTrigger": (0xE, "B"),
       "rightTrigger": (0xF, "B"), "thumbLX": (0x10, "h"), "thumbLY": (0x12, "h"), "thumbRX": (0x14, "h"), "thumbRY": (0x16, "h"),
       "camYaw": (0x18, "f"), "camPitch": (0x1C, "f")}
GUEST = {"seq": (0, "I"), "flags": (4, "I"), "frame": (8, "Q"), "qpc": (0x10, "q"), "heroPos": (0x18, "3d"),
         "heroRot": (0x30, "9f"), "heroVel": (0x54, "3f"), "heroHeight": (0x60, "f"), "teleportAck": (0x64, "I"),
         "camPos": (0x68, "3d"), "camRot": (0x80, "9f"), "camFovYDeg": (0xA4, "f"), "tilesHeld": (0xA8, "I"),
         "tilesQueued": (0xAC, "I"), "heapFreeMB": (0xB0, "I"), "heapUsedMB": (0xB4, "I"), "rescues": (0xB8, "I"),
         "buildsRefused": (0xBC, "I"), "zipFlags": (0xC0, "I"), "zipEdges": (0xC4, "I"), "zipTarget": (0xC8, "3d"),
         "zipStarted": (0xE0, "I"), "zipFailed": (0xE4, "I")}
ZIP_TARGET, ZIP_ACTIVE, ZIP_READY = 1, 2, 4
CAPTURE = {"seq": (0, "I"), "flags": (4, "I"), "generation": (8, "I"), "slot": (0xC, "I"), "frameId": (0x10, "Q"), "qpc": (0x18, "q"),
           "width": (0x20, "I"), "height": (0x24, "I"), "tanHalfFovX": (0x28, "f"), "tanHalfFovY": (0x2C, "f"), "camPos": (0x30, "3d"),
           "camRot": (0x48, "9f"), "range": (0x6C, "f"), "format": (0x70, "I"), "share": (0x74, "I"),
           "handles": (0x78, "4Q")}
CAP_LIVE = 1
TILE_RECORD = {"key": (0, "32s"), "awtBytes": (32, "I"), "awhBytes": (36, "I")}
TILE_RECORD_BYTES = 40
SLOT_BYTES = {"HostState": 0x100, "PadState": 0x100, "GuestState": 0x200, "CaptureState": 0x200}

# XInput buttons
DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT = 0x1, 0x2, 0x4, 0x8
START, BACK, LTHUMB, RTHUMB, LB, RB = 0x10, 0x20, 0x40, 0x80, 0x100, 0x200
A, B, X, Y = 0x1000, 0x2000, 0x4000, 0x8000

_k32 = ctypes.windll.kernel32
_k32.GetTickCount64.restype = ctypes.c_uint64


def now_ms():
	return _k32.GetTickCount64()


def low_priority():
	_k32.SetPriorityClass(_k32.GetCurrentProcess(), 0x40)  # IDLE_PRIORITY_CLASS


class Link:
	def __init__(self):
		self.m = mmap.mmap(-1, MAPPING_BYTES, tagname=MAPPING)
		if self.get(OFF_HEADER, HEADER, "magic") == 0:
			self.set(OFF_HEADER, HEADER, "version", VERSION)
			self.set(OFF_HEADER, HEADER, "magic", MAGIC)
		if self.get(OFF_HEADER, HEADER, "magic") != MAGIC or self.get(OFF_HEADER, HEADER, "version") != VERSION:
			raise RuntimeError("ArkWeb mapping has an incompatible protocol")

	def get(self, base, layout, name):
		off, fmt = layout[name]
		v = struct.unpack_from("<" + fmt, self.m, base + off)
		return v[0] if len(v) == 1 else v

	def set(self, base, layout, name, value):
		off, fmt = layout[name]
		vals = value if isinstance(value, (tuple, list)) else (value,)
		struct.pack_into("<" + fmt, self.m, base + off, *vals)

	# seqlock slot access (single writer per slot)
	def write_slot(self, base, layout, values):
		seq = self.get(base, layout, "seq")
		self.set(base, layout, "seq", (seq + 1) & 0xFFFFFFFF)
		for k, v in values.items():
			self.set(base, layout, k, v)
		self.set(base, layout, "seq", (seq + 2) & 0xFFFFFFFF)

	def read_slot(self, base, layout, tries=50):
		for _ in range(tries):
			s1 = self.get(base, layout, "seq")
			if s1 & 1:
				continue
			out = {k: self.get(base, layout, k) for k in layout}
			if self.get(base, layout, "seq") == s1:
				return out
		return None

	def alive(self, who):
		hb = self.get(OFF_HEADER, HEADER, who + "HeartbeatMs")
		return hb != 0 and now_ms() - hb < HEARTBEAT_TIMEOUT_MS

	def beat(self, who):
		self.set(OFF_HEADER, HEADER, who + "HeartbeatMs", now_ms())


class RingWriter:
	"""Producer side of an SPSC byte ring (src/common/link.h Ring::Write, same bytes). One writer per
	ring: the streamer owns the host ring and the collision ring."""

	def __init__(self, link, offset, total):
		self.m, self.base, self.cap = link.m, offset, total - RING_DATA

	def _u64(self, off):
		return struct.unpack_from("<Q", self.m, self.base + off)[0]

	def free(self):
		return self.cap - (self._u64(RING_HEAD) - self._u64(RING_TAIL))

	def pending(self):
		return self._u64(RING_HEAD) - self._u64(RING_TAIL)

	def write(self, rtype, payload=b""):
		"""False if there is no room right now (try again later)."""
		need = (8 + len(payload) + 7) & ~7
		if need > self.cap // 2: raise ValueError("record of %d bytes is too big for the ring" % len(payload))
		head, tail = self._u64(RING_HEAD), self._u64(RING_TAIL)
		pos = head % self.cap
		to_end = self.cap - pos
		pad = to_end if need > to_end else 0
		if head + pad + need - tail > self.cap: return False
		data = self.base + RING_DATA
		if pad:
			if pad >= 8: struct.pack_into("<II", self.m, data + pos, REC_PAD, pad - 8)
			head += pad
			pos = 0
		struct.pack_into("<II", self.m, data + pos, rtype, len(payload))
		self.m[data + pos + 8:data + pos + 8 + len(payload)] = payload
		struct.pack_into("<Q", self.m, self.base + RING_HEAD, head + need)  # x86 stores stay in order: the payload is visible first
		return True


def check_layout():
	exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bin", "link_test.exe")
	ref = json.loads(subprocess.run([exe, "--layout"], capture_output=True, text=True).stdout)
	bad = 0
	for struct_name, layout in (("Header", HEADER), ("HostState", HOST), ("PadState", PAD), ("GuestState", GUEST), ("CaptureState", CAPTURE), ("TileRecord", TILE_RECORD)):
		for k, (off, _) in layout.items():
			if ref.get("%s.%s" % (struct_name, k)) != off:
				print("MISMATCH %s.%s: python %#x, C++ %s" % (struct_name, k, off, ref.get("%s.%s" % (struct_name, k))))
				bad += 1
	for k, v in (("kOffHostState", OFF_HOST), ("kOffPad", OFF_PAD), ("kOffGuestState", OFF_GUEST), ("kOffCapture", OFF_CAPTURE), ("kMappingBytes", MAPPING_BYTES), ("kVersion", VERSION),
	             ("kOffHostRing", OFF_HOST_RING), ("kHostRingBytes", HOST_RING_BYTES), ("kOffCollisionRing", OFF_COLLISION_RING),
	             ("kCollisionRingBytes", COLLISION_RING_BYTES)):
		if ref[k] != v:
			print("MISMATCH", k, v, ref[k]); bad += 1
	print("layout OK" if not bad else "%d mismatches" % bad)
	return bad == 0


if __name__ == "__main__" and "--check" in sys.argv:
	sys.exit(0 if check_layout() else 1)
