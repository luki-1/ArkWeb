"""Stand-in for the Arkham Knight host: keeps the link alive (so the guest fakes focus), plays
scripted virtual-pad input into Spider-Man, and records what the guest publishes.

  python fake_arkham.py monitor [--seconds N]      heartbeat only; log the hero
  python fake_arkham.py handedness                 stick forward / right runs -> guest handedness
  python fake_arkham.py swing [--seconds N]        hold forward + RT, jump every few seconds
Records go to logs/fake_arkham.log and logs/fake_arkham_trace.csv.
"""
import argparse
import math
import os
import time

import arkweb_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "logs")


def log(msg):
	line = time.strftime("%H:%M:%S ") + msg
	print(line, flush=True)
	with open(os.path.join(LOGS, "fake_arkham.log"), "a") as f:
		f.write(line + "\n")


class Host:
	def __init__(self):
		self.link = P.Link()
		self.link.set(P.OFF_HEADER, P.HEADER, "hostPid", os.getpid())
		self.frame = 0
		self.packet = 0
		self.trace = open(os.path.join(LOGS, "fake_arkham_trace.csv"), "w")
		self.trace.write("t,phase,flags,px,py,pz,fx,fy,fz,sx,sy,sz\n")
		self.t0 = time.time()
		self.phase = ""

	def pad(self, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0, buttons=0, active=True):
		self.packet += 1
		self.link.write_slot(P.OFF_PAD, P.PAD, {"flags": P.PAD_ACTIVE if active else 0, "packet": self.packet, "buttons": buttons,
		                                        "leftTrigger": lt, "rightTrigger": rt, "thumbLX": lx, "thumbLY": ly, "thumbRX": rx, "thumbRY": ry})

	def tick(self):
		"""60 Hz: heartbeat + HostState, sample the guest. Returns the guest state (or None)."""
		self.frame += 1
		self.link.write_slot(P.OFF_HOST, P.HOST, {"flags": P.HOST_IN_GAME, "frame": self.frame, "deltaTime": 1 / 60})
		self.link.beat("host")
		g = self.link.read_slot(P.OFF_GUEST, P.GUEST)
		if g and g["flags"] & P.GUEST_IN_WORLD and self.link.alive("guest"):
			r = g["heroRot"]
			self.trace.write("%.3f,%s,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n" % (time.time() - self.t0, self.phase, g["flags"], *g["heroPos"], *r[6:9], *r[0:3]))
			return g
		return None

	def run(self, seconds, phase, **padargs):
		"""Hold a pad state for `seconds`, ticking. Returns the guest states seen."""
		self.phase = phase
		self.pad(**padargs)
		out, end = [], time.time() + seconds
		while time.time() < end:
			g = self.tick()
			if g: out.append((time.time(), g))
			time.sleep(1 / 60)
		return out

	def wait_guest(self, timeout=600):
		log("waiting for the guest (load a free-roam save in Spider-Man)...")
		end = time.time() + timeout
		while time.time() < end:
			if self.tick():
				g = self.link.read_slot(P.OFF_GUEST, P.GUEST)
				log("guest in world: hero at %.2f %.2f %.2f, focus faked: %s" % (*g["heroPos"], bool(g["flags"] & P.GUEST_FOCUS_FAKED)))
				return True
			time.sleep(1 / 60)
		log("no guest")
		return False


def displacement(samples, first_seconds):
	if len(samples) < 2: return None
	t0 = samples[0][0]
	early = [g for t, g in samples if t - t0 <= first_seconds]
	if len(early) < 2: return None
	a, b = early[0]["heroPos"], early[-1]["heroPos"]
	return (b[0] - a[0], b[1] - a[1], b[2] - a[2])


def handedness(h):
	votes = []
	for rep in range(3):
		h.run(1.5, "neutral")
		f = displacement(h.run(1.2, "forward", ly=32767), 0.7)
		h.run(1.5, "neutral")
		r = displacement(h.run(1.2, "right", lx=32767), 0.7)
		if not f or not r or math.hypot(f[0], f[2]) < 0.3 or math.hypot(r[0], r[2]) < 0.3:
			log("rep %d: hero didn't move enough (forward %s, right %s)" % (rep, f, r)); continue
		# right-handed: right = forward x up = (-f.z, 0, f.x) ... with up = +Y: f x (0,1,0) = (-f.z, 0, f.x)
		c = (-f[2], 0.0, f[0])
		dot = (c[0] * r[0] + c[2] * r[2]) / (math.hypot(*c) * math.hypot(r[0], r[2]))
		votes.append(dot)
		log("rep %d: forward run %.2f,%.2f  right run %.2f,%.2f  (forward x up) . right = %+.2f" % (rep, f[0], f[2], r[0], r[2], dot))
	h.run(0.5, "neutral", active=False)
	if votes:
		avg = sum(votes) / len(votes)
		verdict = "RIGHT-handed (coords.h kGuestRightHanded = true is correct)" if avg > 0.5 else "LEFT-handed (set kGuestRightHanded = false)" if avg < -0.5 else "inconclusive"
		log("handedness: mean %+.2f -> %s" % (avg, verdict))


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("mode", choices=["monitor", "handedness", "swing"])
	ap.add_argument("--seconds", type=float, default=30)
	args = ap.parse_args()
	P.low_priority()
	h = Host()
	if not h.wait_guest():
		return
	if args.mode == "monitor":
		end = time.time() + args.seconds
		while time.time() < end:
			s = h.run(5, "monitor", active=False)
			if s: log("hero %.2f %.2f %.2f, %d samples in 5 s" % (*s[-1][1]["heroPos"], len(s)))
			else: log("no guest samples in 5 s")
	elif args.mode == "handedness":
		handedness(h)
	elif args.mode == "swing":
		end, n = time.time() + args.seconds, 0
		start = None
		while time.time() < end:
			s = h.run(0.15, "swing-jump", ly=32767, rt=255, buttons=P.A)
			s += h.run(2.5, "swing", ly=32767, rt=255)
			if s:
				start = start or s[0][1]["heroPos"]
				n += len(s)
				g = s[-1][1]
				log("hero %.1f %.1f %.1f  speed %.1f m/s" % (*g["heroPos"], math.hypot(*g["heroVel"])))
		h.run(0.5, "release", active=False)
		if start:
			g = h.link.read_slot(P.OFF_GUEST, P.GUEST)
			log("swing test: %d samples, moved %.1f m from start" % (n, math.dist(start, g["heroPos"])))
	h.pad(active=False)
	h.trace.close()


if __name__ == "__main__":
	main()
