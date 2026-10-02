"""Stand-in for the Spider-Man guest: replays the recorded swing (recon/sm_swing_trace_1hz.csv) into
the link at 60 Hz, so the Arkham Knight host can be developed with only Arkham running.

The trace is placed where Batman is when the host appears: same horizontal start, and never below
his current height. Usage: python fake_spidey.py [--seconds N] [--speed X] [--no-wait]
"""
import argparse
import csv
import math
import os
import time

import arkweb_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "..", "logs", "fake_spidey.log")


def log(msg):
	line = time.strftime("%H:%M:%S ") + msg
	print(line, flush=True)
	with open(LOG, "a") as f:
		f.write(line + "\n")


def load_trace():
	rows = list(csv.DictReader(open(os.path.join(HERE, "..", "recon", "sm_swing_trace_1hz.csv"))))
	pts = [(float(r["px"]), float(r["py"]), float(r["pz"])) for r in rows]
	fwd = [(float(r["r20"]), float(r["r21"]), float(r["r22"])) for r in rows]
	# keep the moving part only
	moving = [i for i in range(1, len(pts)) if math.dist(pts[i], pts[i - 1]) > 0.5]
	a, b = max(0, moving[0] - 1), min(len(pts), moving[-1] + 2)
	return pts[a:b], fwd[a:b]


def lerp(a, b, t):
	return tuple(x + (y - x) * t for x, y in zip(a, b))


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: one full replay)")
	ap.add_argument("--speed", type=float, default=1.0)
	ap.add_argument("--no-wait", action="store_true", help="don't wait for the host before starting")
	args = ap.parse_args()
	P.low_priority()
	link = P.Link()
	link.set(P.OFF_HEADER, P.HEADER, "guestPid", os.getpid())
	pts, fwd = load_trace()
	log("fake_spidey: %d trace points (%.0f s)" % (len(pts), len(pts) / args.speed))

	anchor = (0.0, 0.0, 0.0)
	waited = time.time()
	while not args.no_wait and not link.alive("host"):
		if time.time() - waited > 600:
			log("no host after 10 minutes, giving up"); return
		link.beat("guest")  # let the host see us while we wait (not in world yet)
		time.sleep(0.25)
	if link.alive("host"):
		hs = link.read_slot(P.OFF_HOST, P.HOST) or {}
		pp = hs.get("puppetPos", (0, 0, 0))
		lowest = min(p[1] for p in pts)
		anchor = (pp[0] - pts[0][0], pp[1] - lowest + 2.0, pp[2] - pts[0][2])
		log("host linked; Batman's feet at guest %.1f %.1f %.1f m -> trace offset %.1f %.1f %.1f" % (*pp, *anchor))

	start = time.time()
	frame, last = 0, None
	duration = args.seconds or (len(pts) - 1) / args.speed
	while time.time() - start < duration:
		t = ((time.time() - start) * args.speed) % (len(pts) - 1)
		i, f = int(t), t - int(t)
		p = tuple(x + o for x, o in zip(lerp(pts[i], pts[i + 1], f), anchor))
		fw = lerp(fwd[i], fwd[i + 1], f)
		n = math.hypot(fw[0], fw[2]) or 1.0
		fw = (fw[0] / n, 0.0, fw[2] / n)
		side = (fw[2], 0.0, -fw[0])  # up x forward (right-handed guest)
		vel = (0.0, 0.0, 0.0) if last is None else tuple((a - b) * 60 for a, b in zip(p, last))
		last = p
		frame += 1
		link.write_slot(P.OFF_GUEST, P.GUEST, {
			"flags": P.GUEST_IN_WORLD, "frame": frame, "qpc": int(time.perf_counter() * 1e7), "heroPos": p,
			"heroRot": (*side, 0.0, 1.0, 0.0, *fw), "heroVel": vel, "heroHeight": 1.78})
		link.beat("guest")
		if frame % 300 == 0:
			hs = link.read_slot(P.OFF_HOST, P.HOST) or {}
			drive = "driving Batman" if hs.get("flags", 0) & P.HOST_DRIVE_PUPPET else "NOT driving"
			pp = hs.get("puppetPos", (0, 0, 0))
			err = math.dist(pp, p) if link.alive("host") else float("nan")
			log("t=%5.1fs hero %.1f %.1f %.1f | host %s, Batman %.1f m from hero" % (time.time() - start, *p, drive if link.alive("host") else "absent", err))
		time.sleep(1 / 60)
	link.write_slot(P.OFF_GUEST, P.GUEST, {"flags": 0, "frame": frame + 1})
	log("fake_spidey: done (%d frames)" % frame)


if __name__ == "__main__":
	main()
