"""cap_view.py: look at a Spider-Man frame dump (`cap dump` in the guest -> logs/capture/).

Writes color.png, depth.png (log scale) and cut.png (what the capture keeps: pixels nearer than the
range, the rest dark) next to the dump, and prints depth statistics, the hero's distance from the
camera, a near-plane estimate and field-of-view candidates from the camera manager's bytes.

  python cap_view.py [--near N] [--reversed 0|1] [--range M]
"""
import argparse
import os
import struct

import numpy as np
from PIL import Image

import pe_tools  # noqa: F401  (idle priority, one core)

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "capture")


def set_dir(d):
	global DIR
	DIR = d


def read_meta():
	meta = {}
	for line in open(os.path.join(DIR, "meta.txt")):
		parts = line.split()
		if parts:
			meta[parts[0]] = parts[1:]
	return meta


def decode_color(raw, w, h, fmt, pitch):
	raw = raw + bytes(max(0, pitch * h - len(raw)))  # the last row is unpadded
	rows = np.frombuffer(raw, np.uint8)[:pitch * h].reshape(h, pitch)
	if fmt in (28, 29):  # R8G8B8A8
		return rows[:, :w * 4].reshape(h, w, 4)[:, :, :3].copy()
	if fmt in (87, 91):  # B8G8R8A8
		return rows[:, :w * 4].reshape(h, w, 4)[:, :, 2::-1].copy()
	if fmt == 24:  # R10G10B10A2
		v = rows[:, :w * 4].copy().view(np.uint32).reshape(h, w)
		rgb = np.stack([v & 1023, (v >> 10) & 1023, (v >> 20) & 1023], -1)
		return (rgb * (255.0 / 1023.0)).astype(np.uint8)
	if fmt == 10:  # R16G16B16A16_FLOAT
		v = rows[:, :w * 8].copy().view(np.float16).reshape(h, w, 4)[:, :, :3].astype(np.float32)
		return (np.clip(v, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
	raise SystemExit("colour format %d not handled" % fmt)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--near", type=float)
	ap.add_argument("--reversed", type=int)
	ap.add_argument("--range", type=float)
	ap.add_argument("--dir", default=DIR)
	a = ap.parse_args()
	set_dir(a.dir)
	m = read_meta()
	cw, ch, cfmt, cpitch = int(m["color"][0]), int(m["color"][1]), int(m["color"][3]), int(m["color"][5])
	dw, dh, dfmt, dpitch = int(m["depth"][0]), int(m["depth"][1]), int(m["depth"][3]), int(m["depth"][5])
	near = a.near if a.near is not None else float(m["near"][0])
	rev = a.reversed if a.reversed is not None else int(m["near"][2])
	rng = a.range if a.range is not None else float(m["near"][4])
	print("colour %dx%d format %d, depth %dx%d format %d; near %g reversed %d range %g" % (cw, ch, cfmt, dw, dh, dfmt, near, rev, rng))
	vx, vy, vw, vh = [int(v) for v in m["view"][:4]] if "view" in m else (0, 0, cw, ch)
	if "view" in m: print("3D view %dx%d at (%d %d); follow camera FOV %.2f deg (as stored)" % (vw, vh, vx, vy, float(m["view"][5]) * 57.2958))

	color = decode_color(open(os.path.join(DIR, "color.bin"), "rb").read(), cw, ch, cfmt, cpitch)
	Image.fromarray(color).save(os.path.join(DIR, "color.png"))
	z = np.frombuffer(open(os.path.join(DIR, "depth.bin"), "rb").read(), np.float32)
	z = np.concatenate([z, np.zeros(max(0, dpitch // 4 * dh - z.size), np.float32)])[:dpitch // 4 * dh].reshape(dh, dpitch // 4)[:, :dw]  # last row unpadded
	nz = z[(z > 0) & (z < 1)]
	print("depth: %.1f%% exactly 0, %.1f%% exactly 1, %d in between" % (100 * np.mean(z == 0), 100 * np.mean(z == 1), nz.size))
	if nz.size:
		print("  raw z percentiles 0.1/1/50/99/99.9: %s" % np.round(np.percentile(nz, [0.1, 1, 50, 99, 99.9]), 7))
	dist = np.where(z > 0, near / np.maximum(z, 1e-12), np.inf) if rev else np.where(z < 1, near / np.maximum(1 - z, 1e-12), np.inf)
	vis = np.clip(np.log10(np.minimum(dist, 1e5)) / 5.0, 0, 1)
	Image.fromarray((255 * (1 - vis)).astype(np.uint8)).save(os.path.join(DIR, "depth.png"))

	# the cut, sampled like the compute pass (depth may be the render resolution)
	color = color[vy:vy + vh, vx:vx + vw]
	ch, cw = color.shape[:2]
	ys = np.minimum(((np.arange(ch) + 0.5) * dh / ch).astype(int), dh - 1)
	xs = np.minimum(((np.arange(cw) + 0.5) * dw / cw).astype(int), dw - 1)
	near_mask = dist[ys][:, xs] < rng
	cut = (color * 0.15).astype(np.uint8)
	cut[near_mask] = color[near_mask]
	Image.fromarray(cut).save(os.path.join(DIR, "cut.png"))
	print("cut keeps %.2f%% of the pixels (range %g m)" % (100 * near_mask.mean(), rng))
	if near_mask.any():
		r, c = np.nonzero(near_mask)
		print("  kept box rows %d..%d cols %d..%d of %dx%d" % (r.min(), r.max(), c.min(), c.max(), ch, cw))

	if "hero" in m and "camera" in m:
		hp = np.array([float(v) for v in m["hero"][1:4]])
		cam = np.array([float(v) for v in m["camera"]]).reshape(4, 4)
		d = np.linalg.norm(hp + np.array([0, 0.9, 0]) - cam[3, :3])
		print("hero %.2f m from the camera (chest)" % d)
		if nz.size and rev:
			print("  near estimate from the nearest pixels: %.4f m" % (np.percentile(nz, 99.9) * (d - 0.4)))
		# where the hero's chest lands for the camera rows (side, up, forward) - with tan(half FOV) unknown,
		# print its view-space angles; compare with the kept box
		v = hp + np.array([0, 0.9, 0]) - cam[3, :3]
		x, y, f = v @ cam[0, :3], v @ cam[1, :3], v @ cam[2, :3]
		print("  chest in view space: side %.2f up %.2f forward %.2f (tan x %.3f, tan y %.3f)" % (x, y, f, x / f, y / f))
		if near_mask.any():
			cy = (r.min() + r.max()) / 2
			print("  kept box centre row %.0f -> ndc y %.3f; tan(half vFOV) ~ %.3f if the chest is the centre" % (
				cy, 1 - 2 * cy / ch, (y / f) / max(1e-6, 1 - 2 * cy / ch)))

	cm = os.path.join(DIR, "camman.bin")
	if os.path.exists(cm):
		b = open(cm, "rb").read()
		f = struct.unpack("<%df" % (len(b) // 4), b[:len(b) // 4 * 4])
		rad = [(i * 4, v) for i, v in enumerate(f) if 0.6 < v < 1.6]
		deg = [(i * 4, v) for i, v in enumerate(f) if 35 < v < 110 and abs(v - round(v, 3)) < 1e-4]
		print("camera manager floats that could be a FOV (radians 0.6..1.6): %d, e.g. %s" % (len(rad), ["+%x=%.4f" % t for t in rad[:24]]))
		print("  (degrees 35..110): %s" % ["+%x=%.3f" % t for t in deg[:24]])
	print("wrote color.png, depth.png, cut.png to", os.path.abspath(DIR))


if __name__ == "__main__":
	main()
