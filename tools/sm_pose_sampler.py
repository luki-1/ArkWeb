"""sm_pose_sampler.py [seconds] [hz]: records the hero's model-space joint matrices (237 4x4 at the transform
record +0xd8, read-only from outside) to logs/sm_poses.npy while the player moves him, so the joint hierarchy
can be worked out from motion (sm_joint_tree.py). Samples where nothing moved are skipped."""
import ctypes
import os
import sys
import time

import numpy as np

from sm_proc import Proc

k32 = ctypes.windll.kernel32
k32.SetPriorityClass(k32.GetCurrentProcess(), 0x4000)  # below normal next to two games

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
hz = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
N = 237
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "sm_poses.npy")

p = Proc()
samples, last = [], None
hl = p.hero_local()  # once: the guest log is parsed whole for it
t_end = time.time() + secs
while time.time() < t_end:
	ent = p.u64(hl + 8) if hl else 0
	xf = p.u64(ent) if ent else 0
	arr = p.u64(xf + 0xd8) if xf else 0
	r = p.read(arr, 64 * N) if arr else None
	if not r:
		hl = p.hero_local()  # a new hero (a respawn)
	else:
		m = np.frombuffer(r, np.float32).reshape(N, 4, 4).copy()
		if np.all(np.isfinite(m)) and (last is None or np.abs(m - last).max() > 1e-3):
			samples.append(m)
			last = m
	time.sleep(1.0 / hz)
a = np.array(samples)
np.save(OUT, a)
print("%d poses saved to %s" % (len(a), OUT))
