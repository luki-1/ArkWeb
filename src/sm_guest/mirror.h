// Mirror: Spider-Man takes Batman's pose (for combat: Arkham's fight animations on Spider-Man).
//
// The host publishes Batman's skeleton bones in Spider-Man's model space every tick (ak_host/pose.h,
// proto::PoseState). Here, right after the game writes the hero's final model-space joint matrices (237 4x4 at the
// transform record +0xd8; rows = rotation rows, then the position), they are turned so each mapped segment points
// the way Batman's does: shortest arc, the segment's joint and everything under it turning about the joint, his own
// bone lengths kept, trunk before limbs. The writer is exe+1601290 (out joints, pose job), run by the animation
// jobs for every animated character; `mirror watch` (a hardware write breakpoint) found it, 2026-10-02 - after the
// AnimControllerComponent update, where the mirror first went, those writes lasted only part of the frame.
//
// The hero's joint tree is not in memory as a table this code could find: it was worked out from motion (2212
// sampled poses, 2026-10-02: each joint's parent is the earlier joint it keeps a fixed offset to). It belongs to
// the suit it was sampled on, so bone lengths are checked before anything is moved.
#pragma once

#include "zip.h"
#include "../common/link.h"
#include "../common/util.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <tlhelp32.h>

namespace arkweb::mirror
{
	// ---- Spider-Man.exe 4.0630 ----------------------------------------------------------------------------
	constexpr uintptr_t kPoseToModel = 0x1601290;  // (joints out, pose job): evaluates the pose, writes model-space joints (exe+1600770)
	constexpr uint8_t   kPoseToModelSig[20] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54,
		                                        0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20 };
	constexpr int       kCompEntity = 0x8, kRecordJoints = 0xd8;  // [comp +0x8] = entity, [entity] = transform record
	constexpr int       kJoints = 237;

	// the hero's joint tree (parents before children; worked out from motion, see above)
	constexpr int16_t kParent[kJoints] = {
		  0,   0,   0,   1,   1,   2,   4,   0,   0,   8,   0,  10,  11,  12,  13,  14,  15,  14,  14,  13,
		 19,  20,  21,  14,  12,  12,  25,  25,  11,  27,  25,  30,  24,  32,  33,  34,  11,  36,  37,  38,
		 39,  38,  38,  37,  43,  44,  45,  38,  36,  36,  36,  50,  50,  11,  52,  50,  55,  49,  57,  58,
		 11,  60,  27,  11,  63,  52,  64,  11,  67,  68,  69,  70,  71,  72,  73,  74,  75,  75,  77,  78,
		 78,  80,  79,  79,  82,  81,  81,  85,  77,  75,  75,  75,  74,  92,  92,  92,  92,  92,  97,  97,
		 97,  97,  97, 102, 102, 102, 102, 102, 107, 107, 107, 107,  73, 112, 113, 114, 115, 116, 115, 118,
		119, 120, 121, 118, 123, 124, 125, 126, 115, 128, 129, 130, 131, 128, 133, 134, 135, 136, 133, 138,
		139, 140, 115, 114, 143, 114, 145, 145, 145, 145, 145, 143, 144, 115, 153, 154, 113, 156, 157, 158,
		145, 160, 160, 160,  73,  73, 165, 166, 167, 168, 169, 168, 171, 172, 173, 174, 168, 176, 177, 178,
		179, 176, 181, 182, 183, 176, 185, 186, 187, 188, 168, 190, 191, 192, 193, 168, 167, 196, 167, 198,
		198, 198, 198, 198, 196, 197, 168, 206, 207, 166, 209, 210, 211, 198, 213, 213, 213,  73,  92,  73,
		 73,  72, 164, 217,  72, 218, 225, 226,  73,  73,  68,  68,  71, 171,   0,   2, 235,
	};

	// Spider-Man's joints the transfer works on (tree above; +x his left) and Batman's PoseState bones
	constexpr int kHips = 10, kHips2 = 11, kLHip = 12, kLKnee = 13, kLAnkle = 14, kLToe = 15, kRHip = 36, kRKnee = 37, kRAnkle = 38, kRToe = 39;
	constexpr int kSpine[4] = { 67, 69, 71, 73 }, kNeck = 74, kHead = 75;
	constexpr int kLClav = 112, kLSho = 113, kLElb = 114, kLWri = 115, kRClav = 165, kRSho = 166, kRElb = 167, kRWri = 168;
	enum PoseBone : int { bPelvis, bSpine, bSpine1, bSpine2, bSpine3, bNeck, bHead, bLClav, bLUA, bLFA, bLHand, bRClav, bRUA, bRFA, bRHand,
		bLThigh, bLCalf, bLFoot, bRThigh, bRCalf, bRFoot, bLToe, bRToe };
	constexpr float kLegLen = 0.460f + 0.403f;  // his hip -> knee -> ankle
	constexpr int   kTurned[] = { 0, 67, 69, 71, 73, 74, kLClav, kLSho, kLElb, kRClav, kRSho, kRElb, kLHip, kLKnee, kLAnkle, kRHip, kRKnee, kRAnkle };

	// bone lengths (m) of the skeleton the tree is for: another suit's may differ
	struct Len
	{
		int16_t a, b;
		float   len;
	};
	constexpr Len kLens[] = { { 113, 114, 0.257f }, { 114, 115, 0.302f }, { 166, 167, 0.257f }, { 167, 168, 0.302f },
		{ 12, 13, 0.460f }, { 13, 14, 0.403f }, { 36, 37, 0.460f }, { 37, 38, 0.403f } };

	using PoseToModelFn = uint64_t (*)(void*, void*);
	inline PoseToModelFn          g_origPoseToModel = nullptr;
	inline proto::PoseState*      g_pose = nullptr;  // the link's slot (main.cpp)
	inline std::atomic<bool>      g_on{ false };      // `mirror on|off`
	inline std::atomic<bool>      g_combat{ false };  // combat.h: Batman is fighting - mirror, and face his way
	inline std::atomic<float>     g_faceYaw{ 0.0f };  // his facing (guest yaw, coords::GuestYawOfForward)
	inline std::atomic<int>       g_skeleton{ 0 };    // 0 not checked, 1 matches, -1 doesn't (mirror stays off)
	inline std::atomic<uint64_t>  g_applied{ 0 }, g_noPose{ 0 };
	inline std::vector<int16_t>   g_sub[kJoints];      // each turned joint's subtree (itself first)
	inline uint64_t               g_lastHostFrame = 0;
	inline ULONGLONG              g_lastHostChange = 0;

	// ---- the transfer (v2; tools/retarget_v2.py is the same in Python, checked on 589 recorded combat poses) -----
	// v1 aimed each segment from his own hips, which stayed at standing height while Batman's pelvis went down to
	// 0.18 m: in a crouch the legs took Batman's directions at full length and the feet went through the ground
	// ("convulsing and distorted", 2026-10-02). v2: 1. the whole body onto Batman's pelvis frame, the hips where his
	// pelvis is (scaled to Spider-Man's legs); 2. the spine aimed, the chest twisted to his shoulder line, neck and
	// head aimed; 3. each limb's upper segment aimed and twisted so the elbow / knee bends in Batman's plane, the
	// lower one aimed.
	struct V3
	{
		float x, y, z;
	};
	inline V3    operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3    operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3    operator*(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
	inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3    Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline float Len3(V3 v) { return std::sqrt(Dot(v, v)); }
	inline V3    Unit(V3 v)
	{
		float n = Len3(v);
		return n > 1e-9f ? v * (1.0f / n) : v;
	}
	inline V3 Sub(const float* a, const float* b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }

	struct M3  // rotation, column vectors: r = q v
	{
		float m[3][3];
	};
	inline V3 Mul(const M3& q, V3 v)
	{
		return { q.m[0][0] * v.x + q.m[0][1] * v.y + q.m[0][2] * v.z, q.m[1][0] * v.x + q.m[1][1] * v.y + q.m[1][2] * v.z,
			q.m[2][0] * v.x + q.m[2][1] * v.y + q.m[2][2] * v.z };
	}

	// turning unit a onto unit b
	inline M3 Arc(V3 a, V3 b)
	{
		a = Unit(a), b = Unit(b);
		V3    v = Cross(a, b);
		float c = Dot(a, b);
		M3    q{};
		if (c < -0.9999f) {  // opposite: half a turn about any perpendicular axis
			V3    p = Unit(std::fabs(a.x) < 0.9f ? Cross(a, { 1, 0, 0 }) : Cross(a, { 0, 1, 0 }));
			float u[3] = { p.x, p.y, p.z };
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j) q.m[i][j] = 2 * u[i] * u[j] - (i == j ? 1.0f : 0.0f);
			return q;
		}
		float k = 1.0f / (1.0f + c);
		float vx[3][3] = { { 0, -v.z, v.y }, { v.z, 0, -v.x }, { -v.y, v.x, 0 } };
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) {
				float vv = 0;
				for (int n = 0; n < 3; ++n) vv += vx[i][n] * vx[n][j];
				q.m[i][j] = (i == j ? 1.0f : 0.0f) + vx[i][j] + vv * k;
			}
		return q;
	}

	inline M3 About(V3 axis, float ang)
	{
		axis = Unit(axis);
		float k[3][3] = { { 0, -axis.z, axis.y }, { axis.z, 0, -axis.x }, { -axis.y, axis.x, 0 } }, sn = std::sin(ang), c = 1 - std::cos(ang);
		M3    q{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) {
				float kk = 0;
				for (int n = 0; n < 3; ++n) kk += k[i][n] * k[n][j];
				q.m[i][j] = (i == j ? 1.0f : 0.0f) + sn * k[i][j] + c * kk;
			}
		return q;
	}

	// columns right, up, forward from an up and a right vector
	inline M3 Frame(V3 up, V3 right)
	{
		V3 u = Unit(up), r = Unit(right - u * Dot(right, u)), f = Cross(r, u);
		return { { { r.x, u.x, f.x }, { r.y, u.y, f.y }, { r.z, u.z, f.z } } };
	}

	inline V3 Pos(float (*a_m)[4][4], int j) { return { a_m[j][3][0], a_m[j][3][1], a_m[j][3][2] }; }

	// a joint and everything under it turned by q about pivot
	inline void Turn(float (*a_m)[4][4], int a_joint, const M3& q, V3 a_pivot)
	{
		for (int16_t x : g_sub[a_joint]) {
			for (int r = 0; r < 3; ++r) {
				V3 v = Mul(q, { a_m[x][r][0], a_m[x][r][1], a_m[x][r][2] });
				a_m[x][r][0] = v.x, a_m[x][r][1] = v.y, a_m[x][r][2] = v.z;
			}
			V3 t = a_pivot + Mul(q, Pos(a_m, x) - a_pivot);
			a_m[x][3][0] = t.x, a_m[x][3][1] = t.y, a_m[x][3][2] = t.z;
		}
	}

	inline void Aim(float (*a_m)[4][4], int a, int b, V3 a_want)
	{
		V3 cur = Pos(a_m, b) - Pos(a_m, a);
		if (Len3(cur) < 0.02f || Len3(a_want) < 1e-6f) return;
		Turn(a_m, a, Arc(cur, a_want), Pos(a_m, a));
	}

	// a's subtree turned about axis (through a) so cur's part across the axis lines up with want's
	inline void Twist(float (*a_m)[4][4], int a, V3 axis, V3 cur, V3 want, float a_min)
	{
		axis = Unit(axis);
		V3 c = cur - axis * Dot(cur, axis), w = want - axis * Dot(want, axis);
		if (Len3(c) < a_min * Len3(cur) || Len3(w) < a_min * Len3(want)) return;
		c = Unit(c), w = Unit(w);
		Turn(a_m, a, About(axis, std::atan2(Dot(Cross(c, w), axis), Dot(c, w))), Pos(a_m, a));
	}

	inline void Limb(float (*a_m)[4][4], const V3* P, int a, int b, int c, int A, int B, int C)
	{
		Aim(a_m, a, b, P[B] - P[A]);
		Twist(a_m, a, P[B] - P[A], Pos(a_m, c) - Pos(a_m, b), P[C] - P[B], 0.15f);
		Aim(a_m, b, c, P[C] - P[B]);
	}

	// a_m: the hero's 237 joint matrices (model space); P: Batman's bones in it
	inline void Retarget(float (*a_m)[4][4], const V3* P)
	{
		// 1. the body onto Batman's pelvis frame, the hips where his pelvis is (scaled to Spider-Man's legs)
		M3 fs = Frame(Pos(a_m, kSpine[3]) - Pos(a_m, kHips2), Pos(a_m, kRHip) - Pos(a_m, kLHip));
		M3 fb = Frame(P[bSpine3] - P[bPelvis], P[bRThigh] - P[bLThigh]);
		M3 q{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) q.m[i][j] = fb.m[i][0] * fs.m[j][0] + fb.m[i][1] * fs.m[j][1] + fb.m[i][2] * fs.m[j][2];  // fb fs^T
		Turn(a_m, 0, q, Pos(a_m, kHips));
		float leg = 0.5f * (Len3(P[bLCalf] - P[bLThigh]) + Len3(P[bLFoot] - P[bLCalf]) + Len3(P[bRCalf] - P[bRThigh]) + Len3(P[bRFoot] - P[bRCalf]));
		float s = leg > 0.3f ? kLegLen / leg : 1.0f;
		V3    shift = P[bPelvis] * s - Pos(a_m, kHips);
		for (int x = 0; x < kJoints; ++x) a_m[x][3][0] += shift.x, a_m[x][3][1] += shift.y, a_m[x][3][2] += shift.z;
		// 2. spine aimed, chest twisted to the shoulder line, neck and head aimed
		Aim(a_m, kSpine[0], kSpine[1], P[bSpine1] - P[bSpine]);
		Aim(a_m, kSpine[1], kSpine[2], P[bSpine2] - P[bSpine1]);
		Aim(a_m, kSpine[2], kSpine[3], P[bSpine3] - P[bSpine2]);
		Twist(a_m, kSpine[3], Pos(a_m, kSpine[3]) - Pos(a_m, kSpine[2]), Pos(a_m, kRSho) - Pos(a_m, kLSho), P[bRUA] - P[bLUA], 0.3f);
		Aim(a_m, kSpine[3], kNeck, P[bNeck] - P[bSpine3]);
		Aim(a_m, kNeck, kHead, P[bHead] - P[bNeck]);
		// 3. limbs
		Aim(a_m, kLClav, kLSho, P[bLUA] - P[bLClav]);
		Limb(a_m, P, kLSho, kLElb, kLWri, bLUA, bLFA, bLHand);
		Aim(a_m, kRClav, kRSho, P[bRUA] - P[bRClav]);
		Limb(a_m, P, kRSho, kRElb, kRWri, bRUA, bRFA, bRHand);
		Limb(a_m, P, kLHip, kLKnee, kLAnkle, bLThigh, bLCalf, bLFoot);
		Aim(a_m, kLAnkle, kLToe, P[bLToe] - P[bLFoot]);
		Limb(a_m, P, kRHip, kRKnee, kRAnkle, bRThigh, bRCalf, bRFoot);
		Aim(a_m, kRAnkle, kRToe, P[bRToe] - P[bRFoot]);
	}

	// Batman's bones turned about the vertical by his facing relative to Spider-Man's (combat: the hero's own
	// facing is whatever his body last had; a_xf: his transform, rows side/up/forward in New York's world)
	inline void FaceBatman(V3* P, uintptr_t a_xf)
	{
		float rows[3][4];
		if (!SafeRead(rows, reinterpret_cast<const void*>(a_xf), sizeof(rows))) return;
		coords::V3 b = coords::GuestForwardOfYaw(g_faceYaw.load(std::memory_order_relaxed));
		float      bm[3];  // Batman's forward in his model frame
		for (int r = 0; r < 3; ++r) bm[r] = static_cast<float>(rows[r][0] * b.x + rows[r][1] * b.y + rows[r][2] * b.z);
		if (std::fabs(bm[0]) + std::fabs(bm[2]) < 1e-3f) return;
		M3 q = About({ 0, 1, 0 }, std::atan2(bm[0], bm[2]));
		for (uint32_t i = 0; i < proto::kPoseBones; ++i) P[i] = Mul(q, P[i]);
	}

	inline bool CheckSkeleton(float (*a_m)[4][4])
	{
		for (const Len& l : kLens) {
			float d = Len3(Sub(a_m[l.b][3], a_m[l.a][3]));
			if (std::fabs(d - l.len) > 0.03f) {
				Log("mirror: joint %d -> %d is %.3f m, not %.3f: this suit's skeleton isn't the one the table is for - mirror off", l.a, l.b, d, l.len);
				return false;
			}
		}
		Log("mirror: the hero's skeleton matches (237 joints)");
		return true;
	}

	// a_out: the joint array the pose job just wrote - every animated character's; only the hero's is his
	inline void MirrorJoints(uintptr_t a_out)
	{
		uintptr_t ent = zip::HeroEntity();
		uintptr_t rec = ent ? ReadOr<uintptr_t>(ent, 0) : 0;
		uintptr_t joints = rec ? ReadOr<uintptr_t>(rec + kRecordJoints, 0) : 0;
		if (!joints || a_out != joints || !g_pose) return;
		proto::PoseState ps;
		if (!SeqRead(g_pose, ps, 4) || !(ps.flags & proto::kPoseValid) || ps.count != proto::kPoseBones) {
			++g_noPose;
			return;
		}
		ULONGLONG now = GetTickCount64();
		if (ps.frame != g_lastHostFrame) g_lastHostFrame = ps.frame, g_lastHostChange = now;
		if (now - g_lastHostChange > 500) {  // Arkham paused or gone: his own pose
			++g_noPose;
			return;
		}
		auto* m = reinterpret_cast<float(*)[4][4]>(joints);
		__try {
			if (g_skeleton.load() == 0) g_skeleton = CheckSkeleton(m) ? 1 : -1;
			if (g_skeleton.load() != 1) {
				g_on = false;
				return;
			}
			V3 P[proto::kPoseBones];
			for (uint32_t i = 0; i < proto::kPoseBones; ++i) P[i] = { ps.bone[i][0], ps.bone[i][1], ps.bone[i][2] };
			if (g_combat.load(std::memory_order_relaxed)) FaceBatman(P, rec);
			Retarget(m, P);
			++g_applied;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_on = false;
			Log("mirror: exception writing the hero's joints - mirror off");
		}
	}

	inline uint64_t PoseToModelDetour(void* a_out, void* a_job)
	{
		uint64_t r = g_origPoseToModel(a_out, a_job);
		if (g_on.load(std::memory_order_relaxed) || g_combat.load(std::memory_order_relaxed)) MirrorJoints(reinterpret_cast<uintptr_t>(a_out));
		return r;
	}

	inline void Install(proto::PoseState* a_pose)
	{
		g_pose = a_pose;
		for (int a : kTurned) {
			std::vector<int16_t>& out = g_sub[a];
			if (!out.empty()) continue;
			out.push_back(static_cast<int16_t>(a));
			for (size_t i = 0; i < out.size(); ++i)  // breadth first over the tree
				for (int j = out[i] + 1; j < kJoints; ++j)
					if (kParent[j] == out[i]) out.push_back(static_cast<int16_t>(j));
		}
		g_origPoseToModel = reinterpret_cast<PoseToModelFn>(InlineHook(g_exe + kPoseToModel, kPoseToModelSig, sizeof(kPoseToModelSig), reinterpret_cast<void*>(&PoseToModelDetour)));
		Log("mirror: pose writer hook %s (Spider-Man takes Batman's pose in combat, or with `mirror on`)", g_origPoseToModel ? "ready" : "FAILED - this build doesn't match");
	}

	// ---- `mirror watch [ms]`: who writes the hero's joints -------------------------------------------------
	// The mirror's writes didn't last the frame (2026-10-02: the left forearm matched Batman's 20% of the time):
	// something writes the joints again after the animation update. A hardware data breakpoint on joint 115's
	// position (all threads, writes, 8 bytes) catches every writer for a moment; a vectored handler keeps the
	// instruction address and the code addresses on its stack (return-address candidates), logged afterwards.
	struct WatchHit
	{
		DWORD     tid;
		uintptr_t rip;
		uintptr_t ret[8];
	};
	inline WatchHit              g_hits[64];
	inline std::atomic<int>      g_hitCount{ 0 };
	inline std::atomic<uintptr_t> g_watchAddr{ 0 };
	inline std::atomic<bool>     g_watchSession{ false };  // breakpoint exceptions are ours (ends well after they are cleared)
	inline PVOID                 g_veh = nullptr;

	inline LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* a_e)
	{
		// one that trips while the breakpoints are being cleared is ours too: unhandled, it crashed the game
		// (2026-10-02, "Single step" in the pose writer)
		if (a_e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watchSession.load() || !(a_e->ContextRecord->Dr6 & 1))
			return EXCEPTION_CONTINUE_SEARCH;
		CONTEXT* c = a_e->ContextRecord;
		int      n = g_hitCount.fetch_add(1);
		if (n < 64) {
			WatchHit& h = g_hits[n];
			h.tid = GetCurrentThreadId();
			h.rip = c->Rip;
			int k = 0;
			for (uintptr_t sp = c->Rsp; sp < c->Rsp + 0x600 && k < 8; sp += 8) {
				uintptr_t v = 0;
				if (SafeRead(&v, reinterpret_cast<const void*>(sp), 8) && v > g_exe + 0x1000 && v < g_exe + 0x5000000) h.ret[k++] = v;
			}
			for (; k < 8; ++k) h.ret[k] = 0;
		}
		c->Dr6 = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	// a_addr 0: off. Every thread of the game but the calling one; a_len 4 or 8 bytes (the address aligned to it).
	inline int SetWatch(uintptr_t a_addr, int a_len = 8)
	{
		const unsigned long long lenBits = a_len == 8 ? 2ull : 3ull;  // DR7 LEN0: 10 = 8 bytes, 11 = 4 bytes
		if (a_addr) g_watchSession = true;
		g_watchAddr = a_addr;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return 0;
		THREADENTRY32 te{ sizeof(te) };
		int           n = 0;
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
			if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
			HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
			if (!t) continue;
			if (SuspendThread(t) != static_cast<DWORD>(-1)) {
				CONTEXT ctx{};
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(t, &ctx)) {
					ctx.Dr0 = a_addr;
					ctx.Dr7 = a_addr ? (ctx.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (lenBits << 18) : (ctx.Dr7 & ~0xF0003ull);  // L0, writes
					ctx.Dr6 = 0;
					if (SetThreadContext(t, &ctx)) ++n;
				}
				ResumeThread(t);
			}
			CloseHandle(t);
		}
		CloseHandle(snap);
		return n;
	}

	struct WatchRequest
	{
		uintptr_t addr;
		int       len;
		DWORD     ms;
		char      label[48];
	};

	inline DWORD WINAPI WatchThread(void* a_req)
	{
		WatchRequest r = *static_cast<WatchRequest*>(a_req);
		delete static_cast<WatchRequest*>(a_req);
		if (!g_veh) g_veh = AddVectoredExceptionHandler(1, WatchHandler);
		g_hitCount = 0;
		int threads = SetWatch(r.addr, r.len);
		Sleep(r.ms);
		SetWatch(0);
		Sleep(500);  // any breakpoint exception still on its way is handled before the session ends
		g_watchSession = false;
		int n = g_hitCount.load();
		Log("mirror watch: %d writes to %s (%p, %d bytes) in %lu ms, %d threads watched (mirror %s)", n, r.label, reinterpret_cast<void*>(r.addr), r.len, r.ms,
			threads, g_on || g_combat ? "on" : "off");
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WatchHandler), &self);
		for (int i = 0; i < n && i < 64; ++i) {
			const WatchHit& h = g_hits[i];
			char rets[160] = {}, *w = rets;
			for (int k = 0; k < 8 && h.ret[k]; ++k) w += snprintf(w, rets + sizeof(rets) - w, " %llx", static_cast<unsigned long long>(h.ret[k] - g_exe));
			bool ours = self && InModule(h.rip, reinterpret_cast<uintptr_t>(self));
			Log("   %2d thread %5lu rip %s+%llx  stack:%s", i, h.tid, ours ? "ArkWeb" : "exe",
				static_cast<unsigned long long>(h.rip - (ours ? reinterpret_cast<uintptr_t>(self) : g_exe)), rets);
		}
		return 0;
	}

	// one watch at a time, of a_len (4 or 8) bytes at a_addr for a_ms
	inline void WatchAt(uintptr_t a_addr, int a_len, DWORD a_ms, const char* a_label)
	{
		if (!a_addr || g_watchSession.load()) {
			Log("  mirror watch: %s", a_addr ? "one is running" : "nothing to watch");
			return;
		}
		auto* r = new WatchRequest{ a_addr, a_len, a_ms, {} };
		snprintf(r->label, sizeof(r->label), "%s", a_label);
		if (HANDLE h = CreateThread(nullptr, 0, WatchThread, r, 0, nullptr)) CloseHandle(h);
		else delete r;
	}

	// `mirror on|off|status|watch [ms]`
	inline void Command(const char* a_verb, const char* a_arg = nullptr)
	{
		if (a_verb && !strcmp(a_verb, "watch")) {  // joint 115 (left wrist): its position
			uintptr_t ent = zip::HeroEntity();
			uintptr_t rec = ent ? ReadOr<uintptr_t>(ent, 0) : 0;
			uintptr_t joints = rec ? ReadOr<uintptr_t>(rec + kRecordJoints, 0) : 0;
			WatchAt(joints ? joints + 115 * 64 + 48 : 0, 8, a_arg ? strtoul(a_arg, nullptr, 10) : 300, "joint 115's position");
			return;
		}
		std::string v = a_verb ? a_verb : "status";
		if (v == "on") {
			if (!g_origPoseToModel) Log("  mirror: no hook");
			else if (g_skeleton.load() < 0) Log("  mirror: this suit's skeleton doesn't match the table");
			else g_on = true;
		} else if (v == "off") {
			g_on = false;
		} else if (v != "status") {
			Log("  usage: mirror on | off | status");
			return;
		}
		proto::PoseState ps{};
		bool             have = g_pose && SeqRead(g_pose, ps, 4) && (ps.flags & proto::kPoseValid);
		Log("  mirror %s; skeleton %s; Batman's pose %s (host frame %llu); applied %llu frames, %llu without a pose", g_on ? "on" : "off",
			g_skeleton.load() > 0 ? "matches" : g_skeleton.load() < 0 ? "DOESN'T MATCH" : "not checked yet", have ? "coming" : "none",
			static_cast<unsigned long long>(ps.frame), static_cast<unsigned long long>(g_applied.load()), static_cast<unsigned long long>(g_noPose.load()));
	}
}
