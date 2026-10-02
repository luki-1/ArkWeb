"""Geofence for the New York filter: ON while the hero is inside the loaded Gotham patch, OFF when he
leaves it or falls below the lowest loaded hull (so he never drops out of the world).

  python fall_guard.py [minutes] [inner radius m] [outer radius m]
"""
import os
import sys
import time

import numpy as np

import pe_tools  # noqa: F401
import gotham_overlay as g
from sm_proc import Proc


def main():
	minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 60
	inner = float(sys.argv[2]) if len(sys.argv) > 2 else 85
	outer = float(sys.argv[3]) if len(sys.argv) > 3 else 95
	loads = g.loads()
	center = loads[-1][2]  # the latest load; older copies elsewhere don't count
	near = [(fn, lim, o) for fn, lim, o in loads if np.linalg.norm(o - center) < 1.0]
	floor = min(o[1] + min(h[:, 1].min() for h in g.load_hulls(os.path.join(g.LOGS, fn), lim)) for fn, lim, o in near) - 5
	cmd = os.path.join(g.LOGS, "sm_cmd.txt")

	def send(text):
		while os.path.exists(cmd): time.sleep(0.05)
		open(cmd, "w").write(text)

	p = Proc()
	send("gotham slot4 off\ngotham filter off\n")
	on = False
	print("patch center %s, on inside %.0f m, off outside %.0f m or below y %.1f" % (np.round(center, 1), inner, outer, floor), flush=True)
	end = time.time() + 60 * minutes
	while time.time() < end:
		m = p.hero_matrix()
		if m is None:
			time.sleep(0.5); continue
		pos = m[3, :3]
		flat = np.hypot(pos[0] - center[0], pos[2] - center[2])
		# back on only near Gotham's own level (not from a New York street far below it)
		if not on and flat < inner and pos[1] > center[1] - 15:
			send("gotham filter on\n"); on = True
			print(time.strftime("%H:%M:%S"), "entered patch (%.0f m from center): filter ON" % flat, flush=True)
		elif on and (flat > outer or pos[1] < floor):
			send("gotham filter off\n"); on = False
			why = "left patch (%.0f m)" % flat if flat > outer else "fell below Gotham (y %.1f)" % pos[1]
			print(time.strftime("%H:%M:%S"), why + ": filter OFF", flush=True)
		time.sleep(0.1)
	send("gotham filter off\n")


if __name__ == "__main__":
	main()
