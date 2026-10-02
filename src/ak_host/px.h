// PhysX 3.3.1 (PhysX3_x64.dll shipped with Arkham Knight) access without SDK headers.
//
// Virtual slots were recovered from the DLL's own reflection metadata (tools/px_meta.py) and
// checked against the concrete vtables (NpRigidStatic PhysX3+27f2d0, NpShape +281b80):
//   PxPhysics:     getNbScenes 23, getScenes 24
//   PxScene:       getNbActors 17, getActors 18
//   PxRigidActor:  getGlobalPose 20 (returns PxTransform through a hidden pointer), getNbShapes 25, getShapes 26
//   PxShape:       getGeometryType 5, getLocalPose 17 (hidden return)
// Geometry comes from the exported PxShapeGeometryPropertyHelper::getGeometry overloads.
// PxTriangleMesh internals are found at runtime (FindMeshLayout) because its vtable isn't
// reachable statically.
#pragma once

#include "../common/util.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace arkweb::px
{
	struct Vec3
	{
		float x, y, z;
	};
	struct Quat
	{
		float x, y, z, w;
	};
	struct Transform
	{
		Quat q;
		Vec3 p;
	};

	enum GeometryType : int
	{
		eSPHERE = 0, ePLANE = 1, eCAPSULE = 2, eBOX = 3, eCONVEXMESH = 4, eTRIANGLEMESH = 5, eHEIGHTFIELD = 6,
	};

	// PxGeometry structs (public 3.3 layouts).
	struct MeshScale
	{
		Vec3 scale;
		Quat rotation;
	};
	struct TriangleMeshGeometry
	{
		int       type;
		MeshScale scale;      // +4
		uint8_t   meshFlags;  // +0x20
		uint8_t   pad[7];
		void*     triangleMesh;  // +0x28
	};
	static_assert(sizeof(TriangleMeshGeometry) == 0x30);
	struct BoxGeometry
	{
		int  type;
		Vec3 halfExtents;
	};
	struct ConvexMeshGeometry  // NpShape::getConvexMeshGeometry copies 0x28 bytes, mesh pointer at +0x20
	{
		int       type;
		MeshScale scale;       // +4
		void*     convexMesh;  // +0x20
	};
	static_assert(sizeof(ConvexMeshGeometry) == 0x28);
	struct HeightFieldGeometry
	{
		int     type;
		uint8_t pad[4];
		void*   heightField;  // +8
		float   heightScale, rowScale, columnScale;
		uint8_t heightFieldFlags;
		uint8_t pad2[3];
	};

	using GetPhysicsFn = void* (*)();
	using GetTriMeshGeomFn = bool (*)(const void* a_helper, const void* a_shape, TriangleMeshGeometry& a_out);
	using GetBoxGeomFn = bool (*)(const void* a_helper, const void* a_shape, BoxGeometry& a_out);
	using GetConvexGeomFn = bool (*)(const void* a_helper, const void* a_shape, ConvexMeshGeometry& a_out);
	using GetHfGeomFn = bool (*)(const void* a_helper, const void* a_shape, HeightFieldGeometry& a_out);

	struct Api
	{
		HMODULE          dll = nullptr;
		GetPhysicsFn     getPhysics = nullptr;
		GetTriMeshGeomFn triGeom = nullptr;
		GetBoxGeomFn     boxGeom = nullptr;
		GetConvexGeomFn  convexGeom = nullptr;
		GetHfGeomFn      hfGeom = nullptr;

		bool Load()
		{
			dll = GetModuleHandleW(L"PhysX3_x64.dll");
			if (!dll) return false;
			getPhysics = reinterpret_cast<GetPhysicsFn>(GetProcAddress(dll, "PxGetPhysics"));
			triGeom = reinterpret_cast<GetTriMeshGeomFn>(GetProcAddress(dll,
				"?getGeometry@PxShapeGeometryPropertyHelper@physx@@QEBA_NPEBVPxShape@2@AEAVPxTriangleMeshGeometry@2@@Z"));
			boxGeom = reinterpret_cast<GetBoxGeomFn>(GetProcAddress(dll,
				"?getGeometry@PxShapeGeometryPropertyHelper@physx@@QEBA_NPEBVPxShape@2@AEAVPxBoxGeometry@2@@Z"));
			convexGeom = reinterpret_cast<GetConvexGeomFn>(GetProcAddress(dll,
				"?getGeometry@PxShapeGeometryPropertyHelper@physx@@QEBA_NPEBVPxShape@2@AEAVPxConvexMeshGeometry@2@@Z"));
			hfGeom = reinterpret_cast<GetHfGeomFn>(GetProcAddress(dll,
				"?getGeometry@PxShapeGeometryPropertyHelper@physx@@QEBA_NPEBVPxShape@2@AEAVPxHeightFieldGeometry@2@@Z"));
			return getPhysics && triGeom && boxGeom;
		}
	};

	template <class R, class... A>
	inline R VCall(const void* a_obj, int a_slot, A... a_args)
	{
		auto vt = *reinterpret_cast<void* const* const*>(a_obj);
		return reinterpret_cast<R (*)(const void*, A...)>(vt[a_slot])(a_obj, a_args...);
	}

	inline uint32_t NbScenes(void* a_physics) { return VCall<uint32_t>(a_physics, 23); }
	inline uint32_t Scenes(void* a_physics, void** a_buf, uint32_t a_n) { return VCall<uint32_t>(a_physics, 24, a_buf, a_n, 0u); }

	// PxActorTypeSelectionFlags is a PxFlags<.., PxU16>, which has a user-defined copy constructor,
	// so MSVC passes it BY POINTER to a temporary (PhysX's own accessor: movzx eax, word ptr [rdx]).
	// Passing the value crashed the game (2026-10-01).
	constexpr uint16_t kRigidStatic = 1, kRigidDynamic = 2;
	inline uint32_t NbActors(void* a_scene, uint16_t a_types)
	{
		uint16_t flags = a_types;
		return VCall<uint32_t>(a_scene, 17, &flags);
	}
	inline uint32_t Actors(void* a_scene, uint16_t a_types, void** a_buf, uint32_t a_n, uint32_t a_start)
	{
		uint16_t flags = a_types;
		return VCall<uint32_t>(a_scene, 18, &flags, a_buf, a_n, a_start);
	}

	inline Transform GlobalPose(void* a_actor)
	{
		Transform t{};
		VCall<Transform*>(a_actor, 20, &t);
		return t;
	}
	inline uint32_t NbShapes(void* a_actor) { return VCall<uint32_t>(a_actor, 25); }
	inline uint32_t Shapes(void* a_actor, void** a_buf, uint32_t a_n) { return VCall<uint32_t>(a_actor, 26, a_buf, a_n, 0u); }

	inline int       GeometryTypeOf(void* a_shape) { return VCall<int>(a_shape, 5); }
	inline Transform LocalPose(void* a_shape)
	{
		Transform t{};
		VCall<Transform*>(a_shape, 17, &t);
		return t;
	}

	// ---- math -------------------------------------------------------------------------------------
	inline Vec3 Rotate(const Quat& q, const Vec3& v)
	{
		// v + 2w(q x v) + 2 q x (q x v)
		Vec3 u{ q.x, q.y, q.z };
		Vec3 c1{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
		Vec3 c2{ u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x };
		return { v.x + 2 * (q.w * c1.x + c2.x), v.y + 2 * (q.w * c1.y + c2.y), v.z + 2 * (q.w * c1.z + c2.z) };
	}
	inline Vec3 Apply(const Transform& t, const Vec3& v)
	{
		Vec3 r = Rotate(t.q, v);
		return { r.x + t.p.x, r.y + t.p.y, r.z + t.p.z };
	}
	inline Transform Mul(const Transform& a, const Transform& b)  // a * b
	{
		Quat q{ a.q.w * b.q.x + a.q.x * b.q.w + a.q.y * b.q.z - a.q.z * b.q.y, a.q.w * b.q.y - a.q.x * b.q.z + a.q.y * b.q.w + a.q.z * b.q.x,
			a.q.w * b.q.z + a.q.x * b.q.y - a.q.y * b.q.x + a.q.z * b.q.w, a.q.w * b.q.w - a.q.x * b.q.x - a.q.y * b.q.y - a.q.z * b.q.z };
		return { q, Apply(a, b.p) };
	}
	// PxMeshScale: vertex' = R^T * S * R * v (scale along a rotated frame)
	inline Vec3 ApplyMeshScale(const MeshScale& s, const Vec3& v)
	{
		Quat inv{ -s.rotation.x, -s.rotation.y, -s.rotation.z, s.rotation.w };
		Vec3 r = Rotate(s.rotation, v);
		r = { r.x * s.scale.x, r.y * s.scale.y, r.z * s.scale.z };
		return Rotate(inv, r);
	}

	// ---- PxTriangleMesh internals (found at runtime) ------------------------------------------------------
	struct MeshLayout
	{
		int  offNbVerts = -1;   // u32 vertex count; u32 triangle count follows
		int  offVerts = -1;     // PxVec3*
		int  offTris = -1;      // index pointer
		bool idx16 = false;     // set per mesh by ReadMesh (validated)
		bool Valid() const { return offNbVerts >= 0; }
	};

	inline bool ValidIndices(uintptr_t a_tris, uint32_t a_nT, uint32_t a_nV, bool a_16)
	{
		for (uint32_t s = 0; s < 64; ++s) {
			uint32_t t = static_cast<uint32_t>((static_cast<uint64_t>(a_nT - 1) * s) / 63);
			for (int k = 0; k < 3; ++k) {
				uint32_t i = 0;
				if (a_16) {
					uint16_t v;
					if (!Read(a_tris + (t * 3ull + k) * 2, v)) return false;
					i = v;
				} else if (!Read(a_tris + (t * 3ull + k) * 4, i)) {
					return false;
				}
				if (i >= a_nV) return false;
			}
		}
		return true;
	}

	inline bool ValidVerts(uintptr_t a_verts, uint32_t a_nV)
	{
		for (uint32_t s = 0; s < 32; ++s) {
			uint32_t i = static_cast<uint32_t>((static_cast<uint64_t>(a_nV - 1) * s) / 31);
			Vec3     v;
			if (!Read(a_verts + i * 12ull, v)) return false;
			if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || std::fabs(v.x) > 1e6f || std::fabs(v.y) > 1e6f || std::fabs(v.z) > 1e6f) return false;
		}
		return true;
	}

	// Looks for {u32 nbVerts, u32 nbTris} followed (within 0x40 bytes) by a vertex and an index pointer.
	inline MeshLayout FindMeshLayout(uintptr_t a_mesh)
	{
		MeshLayout l;
		for (int o = 8; o < 0x180; o += 4) {
			uint32_t n[2];
			if (!Read(a_mesh + o, n)) continue;
			if (n[0] < 3 || n[0] > 4000000 || n[1] < 1 || n[1] > 8000000) continue;
			for (int pv = (o + 8 + 7) & ~7; pv < o + 0x48; pv += 8) {
				uintptr_t verts = ReadOr<uintptr_t>(a_mesh + pv, 0);
				if (verts < 0x10000 || !ValidVerts(verts, n[0])) continue;
				for (int pt = pv + 8; pt < o + 0x50; pt += 8) {
					uintptr_t tris = ReadOr<uintptr_t>(a_mesh + pt, 0);
					if (tris < 0x10000) continue;
					if (ValidIndices(tris, n[1], n[0], false) || ValidIndices(tris, n[1], n[0], true)) {
						l.offNbVerts = o, l.offVerts = pv, l.offTris = pt;
						return l;
					}
				}
			}
		}
		return l;
	}

	// ---- PxConvexMesh internals (found at runtime) -----------------------------------------------------
	// Gu::ConvexHullData: PxBounds3 aabb, PxVec3 centerOfMass, u16 nbEdges, u8 nbHullVertices,
	// u8 nbPolygons, HullPolygonData* polygons (20 bytes each); the vertices follow the polygons.
	struct ConvexLayout
	{
		int  offHull = -1;  // offset of the AABB inside the mesh object
		bool Valid() const { return offHull >= 0; }
	};

	inline bool ReadConvexAt(uintptr_t a_mesh, int a_off, std::vector<Vec3>* a_out)
	{
		float b[9];
		if (!Read(a_mesh + a_off, b)) return false;
		for (float f : b) {
			if (!std::isfinite(f) || std::fabs(f) > 1e5f) return false;
		}
		const float tol = 1e-3f + 1e-4f * (std::fabs(b[3] - b[0]) + std::fabs(b[4] - b[1]) + std::fabs(b[5] - b[2]));
		if (b[0] > b[3] || b[1] > b[4] || b[2] > b[5]) return false;
		if (b[3] - b[0] + b[4] - b[1] + b[5] - b[2] <= 0.0f) return false;
		for (int k = 0; k < 3; ++k) {
			if (b[6 + k] < b[k] - tol || b[6 + k] > b[3 + k] + tol) return false;  // center of mass inside
		}
		uint8_t nbV = ReadOr<uint8_t>(a_mesh + a_off + 38, 0), nbP = ReadOr<uint8_t>(a_mesh + a_off + 39, 0);
		if (nbV < 4 || nbP < 4) return false;
		uintptr_t polys = ReadOr<uintptr_t>(a_mesh + ((a_off + 40 + 7) & ~7), 0);
		if (polys < 0x10000) return false;
		std::vector<Vec3> v(nbV);
		if (!SafeRead(v.data(), reinterpret_cast<void*>(polys + nbP * 20ull), nbV * 12ull)) return false;
		for (auto& p : v) {
			if (p.x < b[0] - tol || p.x > b[3] + tol || p.y < b[1] - tol || p.y > b[4] + tol || p.z < b[2] - tol || p.z > b[5] + tol) return false;
		}
		if (a_out) *a_out = std::move(v);
		return true;
	}

	inline ConvexLayout FindConvexLayout(uintptr_t a_mesh)
	{
		ConvexLayout l;
		for (int o = 8; o <= 0x80; o += 4) {
			if (ReadConvexAt(a_mesh, o, nullptr)) {
				l.offHull = o;
				break;
			}
		}
		return l;
	}

	struct MeshData
	{
		std::vector<Vec3>     verts;
		std::vector<uint32_t> tris;  // 3 per triangle
	};

	inline bool ReadMesh(uintptr_t a_mesh, const MeshLayout& a_l, MeshData& a_out)
	{
		uint32_t n[2];
		if (!a_l.Valid() || !Read(a_mesh + a_l.offNbVerts, n) || n[0] < 3 || n[0] > 4000000 || n[1] > 8000000) return false;
		uintptr_t verts = ReadOr<uintptr_t>(a_mesh + a_l.offVerts, 0), tris = ReadOr<uintptr_t>(a_mesh + a_l.offTris, 0);
		bool idx32 = ValidIndices(tris, n[1], n[0], false), idx16 = !idx32 && ValidIndices(tris, n[1], n[0], true);
		if (!idx32 && !idx16) return false;
		a_out.verts.resize(n[0]);
		a_out.tris.resize(n[1] * 3ull);
		if (!SafeRead(a_out.verts.data(), reinterpret_cast<void*>(verts), n[0] * 12ull)) return false;
		if (idx32) return SafeRead(a_out.tris.data(), reinterpret_cast<void*>(tris), n[1] * 12ull);
		std::vector<uint16_t> t16(n[1] * 3ull);
		if (!SafeRead(t16.data(), reinterpret_cast<void*>(tris), n[1] * 6ull)) return false;
		for (size_t i = 0; i < t16.size(); ++i) a_out.tris[i] = t16[i];
		return true;
	}
}
