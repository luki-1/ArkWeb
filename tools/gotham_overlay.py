"""Draw the Gotham collision the guest loaded on top of the Spider-Man window: the walkable (upward
facing) faces of every hull near the hero, outlined and lightly hatched, colored by height relative
to the hero's feet (green = about your level, yellow/red = above, blue = below).

Reads everything from outside the game: hull file + placement from logs/sm_guest.log, hero and
camera transforms with ReadProcessMemory. The window is transparent and click-through.

  python gotham_overlay.py --cam <camera spec> [--file gotham_150m.bin] [--radius 60] [--vfov 55]

Camera spec (from sm_camfind.py): "0x1234" = matrix address, or "0xOBJ+K>OFF" = [OBJ+K] + OFF.
Keys while it runs: PageUp/PageDown = FOV +-2 degrees, End = quit, Home = hide/show.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
import time
import tkinter as tk

import numpy as np
from scipy.spatial import ConvexHull

from sm_proc import LOG, Proc

ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # below normal
LOGS = os.path.dirname(LOG)
user32 = ctypes.windll.user32
KEY = "#010203"  # transparent color


def loads():
	"""[(file, limit, origin)] for every 'gotham load' in this run's guest log, in order."""
	out, pending = [], None
	for line in open(LOG, errors="replace"):
		m = re.search(r"> gotham load (\S+)\s*(\d*)", line)
		if m: pending = (m.group(1), int(m.group(2) or 0))
		m = re.search(r"> gotham tiles (\S+)", line)
		if m: pending = (m.group(1), -1)  # limit -1 marks a tile file
		m = re.search(r"origin placed at ([-\d.]+) ([-\d.]+) ([-\d.]+)", line)
		if m and pending:
			out.append((pending[0], pending[1], np.array([float(x) for x in m.groups()])))
			pending = None
	return out


def placement():
	"""(file, limit, origin) of the first load."""
	l = loads()
	return l[0] if l else (None, 0, None)


def read_hulls(path):
	with open(path, "rb") as f:
		assert f.read(4) == b"AWG1"
		n = struct.unpack("<I", f.read(4))[0]
		hulls = []
		for _ in range(n):
			k = struct.unpack("<I", f.read(4))[0]
			hulls.append(np.frombuffer(f.read(12 * k), np.float32).reshape(-1, 3).astype(np.float64))
	return hulls


def read_tiles(path):
	"""[(center, verts Nx3, tris Mx3)] of an AWT1 tile file (guest meters, relative to its origin)."""
	out = []
	with open(path, "rb") as f:
		assert f.read(4) == b"AWT1"
		n = struct.unpack("<I", f.read(4))[0]
		for _ in range(n):
			cx, cy, cz, nv, nt = struct.unpack("<3fII", f.read(20))
			v = np.frombuffer(f.read(12 * nv), np.float32).reshape(-1, 3).astype(np.float64) + (cx, cy, cz)
			t = np.frombuffer(f.read(6 * nt), np.uint16).reshape(-1, 3).astype(np.int64)
			out.append((np.array([cx, cy, cz]), v, t))
	return out


def tile_faces(path):
	"""Walkable (upward) triangles of a tile file as polygons."""
	polys = []
	for _, v, t in read_tiles(path):
		tri = v[t]
		n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
		n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
		polys += list(tri[np.abs(n[:, 1]) > 0.7])
	return polys


def load_hulls(path, limit):
	if limit < 0:  # tile file: its vertices as point sets (one per tile) for floor / extent queries
		return [v for _, v, _ in read_tiles(path)]
	hulls = read_hulls(path)
	order = np.argsort([float((h.mean(0) ** 2).sum()) for h in hulls], kind="stable")
	return [hulls[i] for i in (order[:limit] if limit else order)]


def top_faces(hulls, cache):
	"""Walkable faces: list of (polygon Nx3, center) per upward-facing hull facet (normal y > 0.7)."""
	if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(cache.split("_top")[0] + ".bin"):
		d = np.load(cache, allow_pickle=True)
		return list(d["polys"])
	polys = []
	for h in hulls:
		try:
			ch = ConvexHull(h)
		except Exception:
			continue
		groups = {}
		for simplex, eq in zip(ch.simplices, ch.equations):
			if eq[1] < 0.7: continue
			key = tuple(np.round(eq, 3))
			groups.setdefault(key, set()).update(simplex.tolist())
		for key, idx in groups.items():
			pts = h[sorted(idx)]
			c = pts.mean(0)
			ang = np.arctan2(pts[:, 2] - c[2], pts[:, 0] - c[0])
			polys.append(pts[np.argsort(ang)])
	np.savez(cache, polys=np.array(polys, dtype=object))
	return polys


def game_rect(pid):
	"""Client rect of the game window while it is in front, (0,0,0,0) while it isn't, None once it's gone.
	The foreground window is used if it belongs to the game process (FindWindow can return a stale
	GameNxApp window, the game recreates it at startup)."""
	fg = user32.GetForegroundWindow()
	owner = wt.DWORD()
	user32.GetWindowThreadProcessId(fg, ctypes.byref(owner))
	if owner.value != pid:
		return (0, 0, 0, 0) if user32.FindWindowW("GameNxApp", None) else None
	r = wt.RECT(); user32.GetClientRect(fg, ctypes.byref(r))
	pt = wt.POINT(0, 0); user32.ClientToScreen(fg, ctypes.byref(pt))
	return pt.x, pt.y, r.right, r.bottom


def find_vtable_object(p, vt_rva):
	"""First heap object whose vtable is exe+vt_rva."""
	vt = np.uint64(p.exe + vt_rva)
	for base, size in p.regions():
		for off in range(0, size, 1 << 24):
			b = p.read(base + off, min(1 << 24, size - off))
			if b is None: continue
			hit = np.nonzero(np.frombuffer(b, np.uint64) == vt)[0]
			if len(hit): return base + off + int(hit[0]) * 8
	return 0


def resolve_cam(p, spec):
	if spec == "auto":  # Hero::HeroCameraManager +0x744: 4x4 camera matrix (rows side/up/fwd/pos)
		obj = find_vtable_object(p, 0x38b1dd0)
		print("camera: HeroCameraManager %x" % obj, flush=True)
		return "0x%x" % (obj + 0x744) if obj else "0x0"
	m = re.fullmatch(r"(0x[0-9a-fA-F]+)\+(0x[0-9a-fA-F]+|\d+)>(0x[0-9a-fA-F]+|\d+)", spec)
	if m:
		obj, k, off = (int(x, 0) for x in m.groups())
		base = p.u64(obj + k)
		return base + off if base else 0
	return int(spec, 0)


def color(dy):
	if dy > 1.5: return "#ffd000" if dy < 8 else "#ff4040"
	if dy < -1.5: return "#40a0ff"
	return "#40ff60"


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--cam", required=True)
	ap.add_argument("--hints", help="hint file (.awh in logs) to outline in cyan, at the first load's origin")
	ap.add_argument("--rope", help="HeroRopeManager address: draws the web from the hero to its anchor (+0x2d0)")
	ap.add_argument("--radius", type=float, default=60)
	ap.add_argument("--vfov", type=float, default=55)
	ap.add_argument("--mirror", action="store_true", help="flip left/right if the drawing is mirrored")
	ap.add_argument("--max", type=int, default=1500, help="most faces drawn per frame")
	a = ap.parse_args()

	polys = []
	for fn, limit, origin in loads():
		path = os.path.join(LOGS, fn)
		faces = tile_faces(path) if limit < 0 else top_faces(load_hulls(path, limit), os.path.splitext(path)[0] + "_top%d.npz" % limit)
		polys += [q + origin for q in faces]
		print("%d walkable faces from %s (limit %d), origin %s" % (len(faces), fn, limit, np.round(origin, 2)), flush=True)
	if not polys: raise SystemExit("no 'gotham load' in the guest log yet")
	centers = np.array([q.mean(0) for q in polys])
	hint_tops = []
	if a.hints:
		with open(os.path.join(LOGS, a.hints), "rb") as f:
			assert f.read(4) == b"AWH1"
			n = struct.unpack("<I", f.read(4))[0]
			h = np.frombuffer(f.read(60 * n), np.float32).reshape(-1, 15).astype(np.float64)
		origin0 = loads()[-1][2]  # hints go with the latest tiles load
		for r in h:
			rows, pos, half = r[:9].reshape(3, 3), r[9:12] + origin0, r[12:15]
			top = pos + rows[1] * half[1]
			c = [top + rows[0] * sx * half[0] + rows[2] * sz * half[2] for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
			hint_tops.append(np.array(c))
		print("%d hint boxes outlined" % len(hint_tops), flush=True)
	hint_centers = np.array([q.mean(0) for q in hint_tops]) if hint_tops else np.zeros((0, 3))

	p = Proc()
	if a.cam == "auto": a.cam = resolve_cam(p, "auto")
	if a.rope == "auto":  # Hero::HeroRopeManager; its web anchor is at +0x2d0
		obj = find_vtable_object(p, 0x38b3df8)
		print("rope: HeroRopeManager %x" % obj, flush=True)
		a.rope = "0x%x" % obj if obj else None
	root = tk.Tk()
	root.overrideredirect(True)
	root.attributes("-topmost", True)
	root.attributes("-transparentcolor", KEY)
	root.config(bg=KEY)
	cv = tk.Canvas(root, bg=KEY, highlightthickness=0)
	cv.pack(fill="both", expand=True)
	root.update()
	hwnd = user32.GetParent(root.winfo_id())
	ex = user32.GetWindowLongW(hwnd, -20)
	user32.SetWindowLongW(hwnd, -20, ex | 0x80000 | 0x20 | 0x80)  # layered, transparent (click-through), toolwindow
	state = {"vfov": a.vfov, "hidden": False, "rect": None, "keys": {}}

	def pressed(vk):
		down = bool(user32.GetAsyncKeyState(vk) & 0x8000)
		was = state["keys"].get(vk, False)
		state["keys"][vk] = down
		return down and not was

	def frame():
		if pressed(0x23): root.destroy(); return  # End
		if pressed(0x24): state["hidden"] = not state["hidden"]  # Home
		if pressed(0x21): state["vfov"] += 2; print("vfov", state["vfov"], flush=True)
		if pressed(0x22): state["vfov"] -= 2; print("vfov", state["vfov"], flush=True)
		rect = game_rect(p.pid)
		if rect is None: root.destroy(); return
		if rect != state["rect"]:
			x, y, w, h = rect
			root.geometry("%dx%d+%d+%d" % (max(w, 1), max(h, 1), x, y))
			state["rect"] = rect
		cv.delete("all")
		hero_m = p.hero_matrix()
		cam_addr = resolve_cam(p, a.cam)
		cm = p.floats(cam_addr, 16) if cam_addr else None
		if state["hidden"] or not state["rect"][2] or hero_m is None or cm is None or not np.all(np.isfinite(cm)):
			state["idle"] = state.get("idle", 0) + 1
			if state["idle"] % 150 == 1:
				print(time.strftime("%H:%M:%S"), "not drawing: hidden %s, rect %s, hero %s, camera %s" % (state["hidden"], state["rect"],
				      hero_m is not None, cm is not None), flush=True)
			root.after(66, frame); return
		cm = cm.reshape(4, 4).astype(np.float64)
		feet = hero_m[3, :3]
		_, _, w, h = state["rect"]
		cam_pos, up, fwd = cm[3, :3], cm[1, :3], cm[2, :3]
		if (feet - cam_pos) @ fwd < 0: fwd = -fwd  # forward is the row that looks at the hero
		right = np.cross(fwd, up) * (-1 if a.mirror else 1)  # right-handed world (handedness test)
		f = 0.5 * h / np.tan(np.radians(state["vfov"]) / 2)
		d = np.linalg.norm(centers - feet, axis=1)
		near = np.nonzero(d < a.radius)[0]
		near = near[np.argsort(d[near])][:a.max]
		for i in near[::-1]:
			q = polys[i] - cam_pos
			z = q @ fwd
			if (z < 0.3).any(): continue
			sx = w / 2 + f * (q @ right) / z
			sy = h / 2 - f * (q @ up) / z
			if sx.max() < 0 or sx.min() > w or sy.max() < 0 or sy.min() > h: continue
			c = color(polys[i][:, 1].max() - feet[1])
			xy = np.stack([sx, sy], 1).ravel().tolist()
			cv.create_polygon(*xy, outline=c, fill=c, stipple="gray12", width=1)
		if len(hint_tops):
			dh = np.linalg.norm(hint_centers - feet, axis=1)
			for i in np.nonzero(dh < a.radius * 1.5)[0]:
				q = hint_tops[i] - cam_pos
				z = q @ fwd
				if (z < 0.3).any(): continue
				sx = w / 2 + f * (q @ right) / z
				sy = h / 2 - f * (q @ up) / z
				cv.create_polygon(*np.stack([sx, sy], 1).ravel().tolist(), outline="#00e5ff", fill="", width=2)
		if a.rope:
			anchor = p.floats(int(a.rope, 0) + 0x2d0, 3)
			if anchor is not None and np.any(anchor != 0):
				pts = np.array([hero_m[3, :3] + hero_m[1, :3] * 1.2, anchor], np.float64) - cam_pos
				z = pts @ fwd
				if (z > 0.3).all():
					sx = w / 2 + f * (pts @ right) / z
					sy = h / 2 - f * (pts @ up) / z
					cv.create_line(sx[0], sy[0], sx[1], sy[1], fill="#ff40ff", width=3)
					cv.create_oval(sx[1] - 8, sy[1] - 8, sx[1] + 8, sy[1] + 8, outline="#ff40ff", width=3)
		state["n"] = state.get("n", 0) + 1
		if state["n"] % 75 == 0:
			print(time.strftime("%H:%M:%S"), "frame %d: rect %s, %d faces near, %d items drawn" % (state["n"], state["rect"], len(near),
			      len(cv.find_all())), flush=True)
		root.after(66, frame)

	frame()
	root.mainloop()


if __name__ == "__main__":
	main()
