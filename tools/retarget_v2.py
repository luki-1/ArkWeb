"""retarget_v2.py: the Batman -> Spider-Man pose transfer, version 2, checked offline on recorded combat
(logs/combat_poses.npz: Batman's 25 PoseState bones per Arkham tick, in Spider-Man's model space, and Spider-Man's
237 joint matrices at the same moments).

v1 only turned the whole body to Batman's facing and aimed each segment: his hips stayed at standing height
(0.99-1.00 m all fight) while Batman's pelvis went from 1.27 m down to 0.18 m, so in a crouch the legs took
Batman's directions at full length and the feet went through the ground; arms and legs could also roll about
their own axis. v2:
  1. the whole body turned onto Batman's pelvis frame (up = pelvis -> Spine3, right = left thigh -> right thigh),
     the hips moved to Batman's pelvis position scaled by the leg-length ratio
  2. spine segments aimed, then the chest twisted about the spine so the shoulder line matches Batman's
  3. each limb: upper segment aimed, then twisted so the elbow / knee bends in Batman's plane, lower segment aimed

  python retarget_v2.py [frame ...]   -> numbers, and logs/retarget_v2.png for the frames given
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "logs")
PAR = None
SUBS = {}

# Spider-Man joints (sm_parents_motion.npy tree; +x his left)
HIPS, HIPS2 = 10, 11
L_HIP, L_KNEE, L_ANKLE, L_TOE = 12, 13, 14, 15
R_HIP, R_KNEE, R_ANKLE, R_TOE = 36, 37, 38, 39
SPINE = [67, 69, 71, 73]
NECK, HEAD = 74, 75
L_CLAV, L_SHO, L_ELB, L_WRI = 112, 113, 114, 115
R_CLAV, R_SHO, R_ELB, R_WRI = 165, 166, 167, 168
# PoseState indices (proto::kPoseBoneNames)
B_PELVIS, B_SPINE, B_SPINE1, B_SPINE2, B_SPINE3, B_NECK, B_HEAD = range(7)
B_LCLAV, B_LUA, B_LFA, B_LHAND, B_RCLAV, B_RUA, B_RFA, B_RHAND = range(7, 15)
B_LTHIGH, B_LCALF, B_LFOOT, B_RTHIGH, B_RCALF, B_RFOOT, B_LTOE, B_RTOE = range(15, 23)
SM_LEG = 0.460 + 0.403


def unit(v):
	n = np.linalg.norm(v)
	return v / n if n > 1e-9 else v


def subtree(root):
	if root not in SUBS:
		out, i = [root], 0
		while i < len(out):
			out += [j for j in range(len(PAR)) if j != out[i] and PAR[j] == out[i]]
			i += 1
		SUBS[root] = out
	return SUBS[root]


def turn(M, joint, Q, pivot):
	"""rotate joint's subtree by Q (column-vector rotation) about pivot"""
	for x in subtree(joint):
		M[x, :3, :3] = M[x, :3, :3] @ Q.T
		M[x, 3, :3] = pivot + Q @ (M[x, 3, :3] - pivot)


def arc(a, b):
	a, b = unit(a), unit(b)
	v = np.cross(a, b)
	c = float(a @ b)
	if c < -0.9999:
		p = unit(np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0]))
		return 2 * np.outer(p, p) - np.eye(3)
	vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
	return np.eye(3) + vx + vx @ vx / (1 + c)


def about(axis, angle):
	axis = unit(axis)
	K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
	return np.eye(3) + np.sin(angle) * K + (1 - np.cos(angle)) * K @ K


def frame(up, right):
	"""orthonormal columns (right, up, forward) from an up and a right vector"""
	u = unit(up)
	r = unit(right - (right @ u) * u)
	return np.stack([r, u, np.cross(r, u)], 1)


def aim(M, a, b, want):
	"""segment a -> b turned onto the direction want (subtree of a about a)"""
	cur = M[b, 3, :3] - M[a, 3, :3]
	if np.linalg.norm(cur) < 0.02 or np.linalg.norm(want) < 1e-6: return
	turn(M, a, arc(cur, want), M[a, 3, :3].copy())


def twist(M, a, axis, cur_dir, want_dir, min_len=0.15):
	"""subtree of a turned about axis (through a) so cur_dir's part across the axis lines up with want_dir's"""
	axis = unit(axis)
	c = cur_dir - (cur_dir @ axis) * axis
	w = want_dir - (want_dir @ axis) * axis
	if np.linalg.norm(c) < min_len * np.linalg.norm(cur_dir) or np.linalg.norm(w) < min_len * np.linalg.norm(want_dir): return
	c, w = unit(c), unit(w)
	ang = np.arctan2(np.cross(c, w) @ axis, c @ w)
	turn(M, a, about(axis, ang), M[a, 3, :3].copy())


def limb(M, P, a, b, c, A, B, C):
	"""upper a -> b aimed at A -> B, twisted so b -> c bends like B -> C, then b -> c aimed at B -> C"""
	aim(M, a, b, P[B] - P[A])
	twist(M, a, P[B] - P[A], M[c, 3, :3] - M[b, 3, :3], P[C] - P[B])
	aim(M, b, c, P[C] - P[B])


def retarget(M0, P):
	M = M0.astype(np.float64).copy()
	P = np.asarray(P, np.float64)
	# 1. the body onto Batman's pelvis frame, the hips where his pelvis is (scaled to Spider-Man's legs)
	FS = frame(M[SPINE[-1], 3, :3] - M[HIPS2, 3, :3], M[R_HIP, 3, :3] - M[L_HIP, 3, :3])
	FB = frame(P[B_SPINE3] - P[B_PELVIS], P[B_RTHIGH] - P[B_LTHIGH])
	Q = FB @ FS.T
	turn(M, 0, Q, M[HIPS, 3, :3].copy())
	bat_leg = 0.5 * (np.linalg.norm(P[B_LCALF] - P[B_LTHIGH]) + np.linalg.norm(P[B_LFOOT] - P[B_LCALF]) +
	                 np.linalg.norm(P[B_RCALF] - P[B_RTHIGH]) + np.linalg.norm(P[B_RFOOT] - P[B_RCALF]))
	s = SM_LEG / bat_leg if bat_leg > 0.3 else 1.0
	shift = s * P[B_PELVIS] - M[HIPS, 3, :3]
	for x in range(len(M)): M[x, 3, :3] += shift
	# 2. spine aimed, chest twisted to the shoulder line, neck and head aimed
	for (a, b), (A, B) in zip(zip(SPINE, SPINE[1:]), zip([B_SPINE, B_SPINE1, B_SPINE2], [B_SPINE1, B_SPINE2, B_SPINE3])):
		aim(M, a, b, P[B] - P[A])
	twist(M, SPINE[-1], M[SPINE[-1], 3, :3] - M[SPINE[-2], 3, :3], M[R_SHO, 3, :3] - M[L_SHO, 3, :3], P[B_RUA] - P[B_LUA], 0.3)
	aim(M, SPINE[-1], NECK, P[B_NECK] - P[B_SPINE3])
	aim(M, NECK, HEAD, P[B_HEAD] - P[B_NECK])
	# 3. limbs
	for clav, sho, elb, wri, CL, UA, FA, HA in ((L_CLAV, L_SHO, L_ELB, L_WRI, B_LCLAV, B_LUA, B_LFA, B_LHAND),
	                                           (R_CLAV, R_SHO, R_ELB, R_WRI, B_RCLAV, B_RUA, B_RFA, B_RHAND)):
		aim(M, clav, sho, P[UA] - P[CL])
		limb(M, P, sho, elb, wri, UA, FA, HA)
	for hip, knee, ankle, toe, TH, CA, FO, TO in ((L_HIP, L_KNEE, L_ANKLE, L_TOE, B_LTHIGH, B_LCALF, B_LFOOT, B_LTOE),
	                                             (R_HIP, R_KNEE, R_ANKLE, R_TOE, B_RTHIGH, B_RCALF, B_RFOOT, B_RTOE)):
		limb(M, P, hip, knee, ankle, TH, CA, FO)
		aim(M, ankle, toe, P[TO] - P[FO])
	return M


def main():
	global PAR
	PAR = np.load(os.path.join(LOGS, "sm_parents_motion.npy")).tolist()
	d = np.load(os.path.join(LOGS, "combat_poses.npz"))
	B, S = d["bat"].astype(np.float64), d["sm"].astype(np.float64)
	pairs = [((L_SHO, L_ELB), (B_LUA, B_LFA)), ((L_ELB, L_WRI), (B_LFA, B_LHAND)), ((R_SHO, R_ELB), (B_RUA, B_RFA)), ((R_ELB, R_WRI), (B_RFA, B_RHAND)),
	         ((L_HIP, L_KNEE), (B_LTHIGH, B_LCALF)), ((L_KNEE, L_ANKLE), (B_LCALF, B_LFOOT)), ((R_HIP, R_KNEE), (B_RTHIGH, B_RCALF)),
	         ((R_KNEE, R_ANKLE), (B_RCALF, B_RFOOT)), ((67, 73), (B_SPINE, B_SPINE3))]
	out, worst, lowest_foot = [], [], []
	for f in range(len(B)):
		R = retarget(S[f], B[f])
		out.append(R)
		dots = [unit(R[b, 3, :3] - R[a, 3, :3]) @ unit(B[f][Bb] - B[f][Ba]) for (a, b), (Ba, Bb) in pairs]
		worst.append(min(dots))
		lowest_foot.append(min(R[L_TOE, 3, 1], R[R_TOE, 3, 1], R[L_ANKLE, 3, 1], R[R_ANKLE, 3, 1]))
	out = np.array(out)
	body = [HIPS, *SPINE, NECK, HEAD, L_SHO, L_ELB, L_WRI, R_SHO, R_ELB, R_WRI, L_KNEE, L_ANKLE, R_KNEE, R_ANKLE]
	step = np.linalg.norm(np.diff(out[:, body, 3, :3], axis=0), axis=2).max(1)
	bstep = np.linalg.norm(np.diff(B[:, :23], axis=0), axis=2).max(1)
	print("%d frames: worst segment match per frame - median %.4f, min %.4f (1 = Batman's direction)" % (len(B), np.median(worst), np.min(worst)))
	print("feet: lowest ankle/toe height median %.2f m, min %.2f m (0 = ground; v1 had his hips fixed at 1 m)" % (np.median(lowest_foot), np.min(lowest_foot)))
	print("frame-to-frame joint jump: Spider-Man max %.2f m (95th %.2f), Batman's own max %.2f m (95th %.2f)" % (step.max(), np.percentile(step, 95),
	      bstep.max() * SM_LEG / 0.86, np.percentile(bstep, 95)))
	frames = [int(x) for x in sys.argv[1:]] or [int(len(B) * k / 5) for k in range(5)]
	import matplotlib
	matplotlib.use("Agg")
	import matplotlib.pyplot as plt
	ak = json.load(open(os.path.join(LOGS, "ak_skeleton.json")))
	bsegs = [(B_PELVIS, B_SPINE), (B_SPINE, B_SPINE1), (B_SPINE1, B_SPINE2), (B_SPINE2, B_SPINE3), (B_SPINE3, B_NECK), (B_NECK, B_HEAD), (B_SPINE3, B_LCLAV),
	         (B_LCLAV, B_LUA), (B_LUA, B_LFA), (B_LFA, B_LHAND), (B_SPINE3, B_RCLAV), (B_RCLAV, B_RUA), (B_RUA, B_RFA), (B_RFA, B_RHAND),
	         (B_PELVIS, B_LTHIGH), (B_LTHIGH, B_LCALF), (B_LCALF, B_LFOOT), (B_LFOOT, B_LTOE), (B_PELVIS, B_RTHIGH), (B_RTHIGH, B_RCALF),
	         (B_RCALF, B_RFOOT), (B_RFOOT, B_RTOE)]
	ssegs = [(j, p) for j, p in enumerate(PAR) if j != p and j not in (1, 2, 3, 4, 5, 6, 7, 8, 9, 234, 235, 236)]
	fig, ax = plt.subplots(2, len(frames), figsize=(3.2 * len(frames), 7))
	for col, f in enumerate(frames):
		for row, (h, name) in enumerate(((0, "front"), (2, "side"))):
			a = ax[row, col]
			for i, j in bsegs:
				a.plot([B[f][i, h], B[f][j, h]], [B[f][i, 1], B[f][j, 1]], "-", color="#888", lw=3, alpha=0.6)
			R = out[f]
			for j, p in ssegs:
				a.plot([R[j, 3, h], R[p, 3, h]], [R[j, 3, 1], R[p, 3, 1]], "-", color="#c00", lw=0.8)
			a.set_aspect("equal")
			a.set_xlim(-1.1, 1.1)
			a.set_ylim(-0.2, 2.0)
			a.set_title("frame %d %s (grey Batman, red Spider-Man)" % (f, name), fontsize=7)
	plt.tight_layout()
	plt.savefig(os.path.join(LOGS, "retarget_v2.png"), dpi=70)
	print("frames", frames, "-> logs/retarget_v2.png")


if __name__ == "__main__":
	main()
