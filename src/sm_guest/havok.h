// Spider-Man.exe 4.0630 Havok (hknp 2017.1) entry points and layouts used to add Gotham as static
// bodies. Everything mirrors TerrainPhysics::CreateTile (exe+1998230), the game's own runtime path
// for static collision; see ArkWeb/PHASE2.md for the derivation.
#pragma once

#include "../common/util.h"

#include <cstdint>

namespace arkweb::hk
{
	// ---- functions (RVAs) -------------------------------------------------------------------------
	constexpr uintptr_t kCreateConvexFromVertices = 0x2e44410;  // hknpConvexShape* (const hkStridedVertices&, float radius, const BuildConfig&)
	constexpr uintptr_t kBuildConfigCtor = 0x2e43670;           // BuildConfig::BuildConfig(BuildConfig*)
	constexpr uintptr_t kBodyCinfoCtor = 0x2e24b30;             // hknpBodyCinfo::hknpBodyCinfo(hknpBodyCinfo*)
	constexpr uintptr_t kMaterialCtor = 0x2e25800;              // hknpMaterial::hknpMaterial(hknpMaterial*)
	constexpr uintptr_t kAddReference = 0x2aee150;              // hkReferencedObject::addReference(obj)
	constexpr uintptr_t kPhysicsSystemCtor = 0x2e400c0;         // hknpPhysicsSystem(this, world, data, const hkTransform*, int flags)
	constexpr uintptr_t kPhysicsSystemAdd = 0x2e410d0;          // hknpPhysicsSystem::addToWorld(this, int, int)
	// hknpWorld::destroyBodies(world, const hknpBodyId*, int count, activation mode), as the
	// hknpPhysicsSystem destructor (exe+2e40450) calls it with the system's body ids (+0x28, count +0x30;
	// ids whose low 24 bits are all set are invalid and skipped).
	constexpr uintptr_t kWorldDestroyBodies = 0x2e50ae0;
	constexpr int       kSystemBodyIds = 0x28, kSystemBodyCount = 0x30;
	// hkReferencedObject::removeReference(obj): refcount in the high 16 bits of +0x10 (low 16 = memory
	// size, 0 = not counted); at zero it calls deleteThis (vtable +0x10). Bodies hold a reference to
	// their shape (hknpBody::setShape exe+2ec65e0).
	constexpr uintptr_t kRemoveReference = 0x2aee3a0;
	constexpr uintptr_t kHkThreadMemoryVtable = 0x51bee58;  // hkThreadMemory: parent allocator at +0x8

	// Compressed mesh shapes, the way the game builds them at runtime (exe+18116d0): the base
	// hknpCompressedMeshShapeCinfo ctor, then the game's own triangle-provider subclass vtable
	// (vertex count +0x48, float3 vertices +0x40, triangle count +0x58, u16[3] indices +0x50, shape tag
	// u16 +0x68, no convex pieces), then the shape ctor on 0x90 bytes of Havok heap (exe+2eb14d0).
	constexpr uintptr_t kCmsCinfoCtor = 0x2e586f0;      // hknpCompressedMeshShapeCinfo::hknpCompressedMeshShapeCinfo(this)
	constexpr uintptr_t kGameCmsCinfoVtable = 0x3d0a628;
	constexpr uintptr_t kCmsCtor = 0x2e61470;           // hknpCompressedMeshShape(this, const hknpCompressedMeshShapeCinfo&)
	constexpr size_t    kCmsSize = 0x90;
	constexpr size_t    kCmsCinfoSize = 0xA0;
	constexpr uintptr_t kHkMemRouterTls = 0x7db4b48;    // TLS index of the thread's hkMemoryRouter
	constexpr uintptr_t kHkMemRouterFallback = 0x7db4b40;

	// ---- globals / data ------------------------------------------------------------------------------
	constexpr uintptr_t kPhysicsInterface = 0x609a570;   // [ ] -> Physics system interface; +0x18 = hknpWorld*
	constexpr uintptr_t kQueryWorld = 0x78939e8;         // hknpWorld* the query hooks see (should be the same)
	constexpr uintptr_t kIdentityTransform = 0x6b14af0;  // hkTransform identity
	constexpr uintptr_t kSystemDataVtable = 0x3d022d8;   // hknpPhysicsSystemData
	constexpr uintptr_t kMaterialFloat = 0x382e120;      // material +0x3C value used for terrain
	constexpr uintptr_t kCinfoFloat70 = 0x382f0e4;       // body cinfo +0x70 value used for terrain
	constexpr uintptr_t kZoneCs = 0x7813960;             // CRITICAL_SECTION the zone loader holds while changing the world
	constexpr uintptr_t kZoneCsGuard = 0x7813990;        // its MSVC static-init guard (> 0 once initialized)

	// Collision filter info (InsomniacCollisionFilter, exe+18060a0; a body's is hknpBody +0x6c):
	//   bits 0-10   surface categories. A query carries a mask (its preset's u16 +6, PhysicsSystemShared
	//               exe+2038c90); it only sees bodies sharing one of the bits (0 on either side: all).
	//               The same 11 bits come back with every hit (NQueryResult hit +4, exe+1818690).
	//   bit 11      query form;  bits 12-15 layer;  bits 21-31 system group
	// Categories, from the presets and the code that reads them:
	//   0x001 0x002 general solid      0x004 ground ("Ground" in the crawl debug print)
	//   0x008 line of sight / perch finding   0x010 swing point validation (SwingPointHunter)
	//   0x020 critter spots, camera source    0x040 camera penetration (CameraPenetrationTest)
	//   0x080 wall run: WallContourProbe, ImmediateWallRunAttachTest (preset 15)
	//   0x100 water (WaterCollideChecker; traversal throws such hits away)
	//   0x200 wall crawl: MoveProcessorGroundWallCrawl (preset 16), WallHunter::ValidatePointForWallCrawl
	//         and ImmediateWallCrawlAttachTest (hit bit 9)
	//   0x400 web lines (SwingCollider line tests, web zip fan probes)
	// TerrainPhysics' 0x243f (layer 2) lacks camera, wall run, water and crawl: with it Gotham's walls
	// were invisible to every wall-run and wall-crawl probe. Gotham gets all of them but water.
	constexpr uint32_t kTerrainFilterInfo = 0x243f;  // collisionFilterInfo TerrainPhysics gives its bodies
	constexpr uint32_t kGothamFilterInfo = 0x26ff;   // terrain's + camera + wall run + wall crawl
	constexpr uint32_t kCategoryMask = 0x7ff;
	constexpr int      kBodyFilterInfo = 0x6c;       // hknpBody::m_collisionFilterInfo
	constexpr int      kBodyShape = 0x60;            // hknpBody::m_shape (confirmed by the filter's calibration)
	constexpr size_t   kBodySize = 0xC0;             // hknpWorld bodies: [world+0x28] + (id & 0xffffff) * 0xC0
	constexpr int      kWorldBodies = 0x28;
	// Query presets: [exe+7c4e130] -> 32-byte entries, count [[exe+62263e0]+4]; u16 +6 category mask,
	// u16 +8 layer mask, u32 +0xc.
	constexpr uintptr_t kQueryPresets = 0x7c4e130, kQueryPresetCount = 0x62263e0;
	// IgCollisionQueryHitCollector: hits at +0x38 (count +0xc), 0x90 each: hknpCollisionResult (0x70;
	// hit body id +0x48), then the game's info for the hit (+0x78 = the 11 category bits).
	constexpr uintptr_t kHitCollectorVtable = 0x3d0a948;

	// ---- layouts -----------------------------------------------------------------------------------------
	constexpr size_t kBodyCinfoSize = 0xB0;
	constexpr size_t kMaterialSize = 0x70;
	constexpr size_t kMotionPropsSize = 0x70;
	constexpr size_t kSystemDataSize = 0x80;
	constexpr size_t kPhysicsSystemSize = 0x50;

	struct HkArray
	{
		void*    data;
		int32_t  size;
		uint32_t capacityAndFlags;  // 0x80000000: Havok must not free data
	};

	struct StridedVertices
	{
		const float* vertices;
		int32_t      count;
		int32_t      striding;
	};

	using CreateConvexFn = void* (*)(const StridedVertices*, float, const void*);
	using CtorFn = void (*)(void*);
	using AddRefFn = void (*)(void*);
	using SystemCtorFn = void* (*)(void* a_this, void* a_world, void* a_data, const void* a_transform, int a_flags);
	using SystemAddFn = void (*)(void* a_system, int a_flags, int a_mode);
	using CmsCtorFn = void* (*)(void* a_this, const void* a_cinfo);
	using DestroyBodiesFn = void (*)(void* a_world, const uint32_t* a_ids, int a_count, int a_mode);
	using RemoveRefFn = void (*)(void* a_obj);

	// Havok heap block (router +0x58 = heap allocator, vtable slot 1 = blockAlloc(size)), as exe+2eb14d0.
	inline void* HavokAlloc(int a_size)
	{
		uint32_t  tls = ReadOr<uint32_t>(g_exe + kHkMemRouterTls, 0xFFFFFFFF);
		uintptr_t router = tls != 0xFFFFFFFF ? reinterpret_cast<uintptr_t>(TlsGetValue(tls)) : 0;
		if (!router) router = ReadOr<uintptr_t>(g_exe + kHkMemRouterFallback, 0);
		uintptr_t heap = ReadOr<uintptr_t>(router + 0x58, 0);
		if (!heap) return nullptr;
		using AllocFn = void* (*)(void*, int);
		auto fn = reinterpret_cast<AllocFn>(ReadOr<uintptr_t>(ReadOr<uintptr_t>(heap, 0) + 8, 0));
		return fn ? fn(reinterpret_cast<void*>(heap), a_size) : nullptr;
	}

	template <class F>
	inline F Fn(uintptr_t a_rva)
	{
		return reinterpret_cast<F>(g_exe + a_rva);
	}

	inline void* World() { return reinterpret_cast<void*>(ReadOr<uintptr_t>(ReadOr<uintptr_t>(g_exe + kPhysicsInterface, 0) + 0x18, 0)); }
	inline void* QueryWorld() { return reinterpret_cast<void*>(ReadOr<uintptr_t>(g_exe + kQueryWorld, 0)); }

	// Static motion properties: [[world+0xa20]+0x30][0]
	inline const void* StaticMotionProperties(void* a_world)
	{
		uintptr_t lib = ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(a_world) + 0xa20, 0);
		return reinterpret_cast<const void*>(ReadOr<uintptr_t>(lib + 0x30, 0));
	}

	inline CRITICAL_SECTION* ZoneLock()
	{
		int32_t guard = ReadOr<int32_t>(g_exe + kZoneCsGuard, 0);
		return guard > 0 ? reinterpret_cast<CRITICAL_SECTION*>(g_exe + kZoneCs) : nullptr;
	}
}
