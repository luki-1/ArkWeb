// Batman's pose for Spider-Man ("mirror", sm_guest/mirror.h): every tick, on the game thread, the bones named
// in proto::kPoseBoneNames go into the link's PoseState in Spider-Man's model space.
//
// Pawn.Mesh +0x38C -> SkeletalMeshComponent: SkeletalMesh +0x280, SpaceBases +0x3F4 (TArray<BoneAtom>, 32 bytes:
// quaternion, translation, scale; component space: X forward, Y right, Z up, UU). The bones are found by name
// once per SkeletalMesh: RefSkeleton +0xDC, TArray<FMeshBone> of 0x30 bytes with the FName at +0x20 (Rocksteady's
// layout, read live 2026-10-02). Only Batman's own pawn, mesh and arrays are read, on the game thread where they
// are alive (an access violation is fatal in Arkham even inside __try), every index checked against the array.
#pragma once

#include "../common/link.h"
#include "../common/util.h"
#include "ue3.h"

#include <cstring>

namespace arkweb::pose
{
	constexpr int kPawnMesh = 0x38C, kMeshSkeletalMesh = 0x280, kMeshSpaceBases = 0x3F4;
	constexpr int kSkelRefSkeleton = 0xDC, kMeshBoneBytes = 0x30, kMeshBoneName = 0x20, kBoneAtomBytes = 0x20;

	inline uintptr_t g_skelMesh = 0;                 // the SkeletalMesh the indices are for
	inline int       g_index[proto::kPoseBones] = {};
	inline int       g_found = 0;
	inline uint64_t  g_published = 0;
	inline bool      g_loggedMissing = false;

	inline void Resolve(uintptr_t a_skelMesh)
	{
		g_skelMesh = a_skelMesh;
		g_found = 0;
		for (int& i : g_index) i = -1;
		ue3::TArrayRaw ref{};
		if (!Read(a_skelMesh + kSkelRefSkeleton, ref) || !ref.data || ref.num <= 0 || ref.num > 2000) return;
		for (int b = 0; b < ref.num; ++b) {
			std::string n = ue3::NameOf(ReadOr<int32_t>(ref.data + b * static_cast<uintptr_t>(kMeshBoneBytes) + kMeshBoneName, -1));
			for (uint32_t k = 0; k < proto::kPoseBones; ++k) {
				if (g_index[k] < 0 && n == proto::kPoseBoneNames[k]) {
					g_index[k] = b;
					++g_found;
				}
			}
		}
		Log("pose: %d of %u bones found in %s's skeleton (%d bones)", g_found, proto::kPoseBones, ue3::NameOf(ue3::NameIndexOf(a_skelMesh)).c_str(), ref.num);
		g_loggedMissing = false;
	}

	// Game thread, every player tick.
	inline void Publish(Link& a_link, uintptr_t a_pawn, uint64_t a_frame)
	{
		uintptr_t mesh = ReadOr<uintptr_t>(a_pawn + kPawnMesh, 0);
		uintptr_t skel = mesh ? ReadOr<uintptr_t>(mesh + kMeshSkeletalMesh, 0) : 0;
		if (!skel) return;
		if (skel != g_skelMesh) Resolve(skel);
		proto::PoseState ps{};
		ps.count = proto::kPoseBones;
		ps.frame = a_frame;
		ue3::TArrayRaw sb{};
		bool ok = g_found == static_cast<int>(proto::kPoseBones) && Read(mesh + kMeshSpaceBases, sb) && sb.data && sb.num > 0;
		for (uint32_t k = 0; ok && k < proto::kPoseBones; ++k) {
			float t[3];
			if (g_index[k] >= sb.num || !Read(sb.data + g_index[k] * static_cast<uintptr_t>(kBoneAtomBytes) + 0x10, t)) {
				ok = false;
				break;
			}
			ps.bone[k][0] = -t[1] / 100.0f;  // his left = -Y
			ps.bone[k][1] = t[2] / 100.0f;   // up = Z
			ps.bone[k][2] = t[0] / 100.0f;   // forward = X
		}
		if (!ok && g_found != static_cast<int>(proto::kPoseBones) && !g_loggedMissing) {
			g_loggedMissing = true;
			Log("pose: Batman's skeleton lacks some of the bones Spider-Man follows - no pose");
		}
		ps.flags = ok ? proto::kPoseValid : 0;
		SeqWrite(a_link.Pose(), ps);
		if (ok) ++g_published;
	}
}
