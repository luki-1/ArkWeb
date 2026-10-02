// Gotham collision inside Spider-Man (Phase 2b).
//
// Convex point sets (guest space, meters) become hknpConvexShapes and static bodies, added in
// batches as hknpPhysicsSystems exactly like TerrainPhysics::CreateTile does it. Work happens on
// the game's main thread at the start of its frame update (FrameUpdate hook), a few dozen shapes
// per frame, holding the zone loader's lock.
//
// The query filter: the hknpCollisionFilter every world query carries is patched at vtable slot 6
// (isCollisionEnabled(queryType, const hknpQueryFilterData&, const hknpBody&)). With the filter ON,
// only bodies whose shape is a Gotham shape pass, so New York disappears for every query. The
// offset of the shape pointer inside hknpBody is found by looking at the first bodies that are ours.
#pragma once

#include "havok.h"
#include "../common/coords.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace arkweb::gotham
{
	struct Hull
	{
		std::vector<float> pts;  // xyz triples, guest space relative to the file's origin
	};

	// ---- state (main thread unless noted) --------------------------------------------------------------
	inline std::vector<Hull> g_pending;  // hulls waiting to be built
	inline size_t            g_nextPending = 0;
	inline double            g_place[3] = {};  // guest position the file's origin goes to
	inline int               g_perFrame = 64;
	inline int               g_batch = 256;     // bodies per hknpPhysicsSystem

	struct Batch
	{
		std::vector<void*>   shapes;
		std::vector<float>   centers;  // xyz per body
	};
	inline Batch g_building;

	inline std::atomic<int>    g_shapesBuilt{ 0 }, g_shapeFailures{ 0 }, g_bodiesAdded{ 0 }, g_systems{ 0 };
	inline std::vector<void*>  g_keepAlive;  // our allocations (never freed while the game runs)

	// Shape-pointer set, insert-only, lock-free reads (filter runs on many threads).
	constexpr size_t                  kSetSize = 1 << 18;
	inline std::atomic<uintptr_t>*    g_shapeSet = nullptr;
	inline std::atomic<int>           g_shapeSetCount{ 0 };

	inline void SetInsert(uintptr_t a_p)
	{
		size_t h = (a_p >> 4) * 0x9E3779B97F4A7C15ull >> 46;
		for (size_t i = 0; i < kSetSize; ++i) {
			auto&     slot = g_shapeSet[(h + i) & (kSetSize - 1)];
			uintptr_t cur = 0;
			if (slot.compare_exchange_strong(cur, a_p) || cur == a_p) {
				++g_shapeSetCount;
				return;
			}
		}
	}

	// Unloaded (streamed) tiles: their shapes are marked dead in place (pointer | 1; shapes are 16-byte
	// aligned) and every query rejects their bodies, whatever the filter mode.
	inline std::atomic<int> g_deadCount{ 0 };

	inline void SetMarkDead(uintptr_t a_p)
	{
		size_t h = (a_p >> 4) * 0x9E3779B97F4A7C15ull >> 46;
		for (size_t i = 0; i < 64; ++i) {
			auto&     slot = g_shapeSet[(h + i) & (kSetSize - 1)];
			uintptr_t cur = a_p;
			if (slot.compare_exchange_strong(cur, a_p | 1)) {
				++g_deadCount;
				return;
			}
			if (cur == 0) return;
		}
	}

	// Replaces an entry with a tombstone (2): probing goes on past it, nothing matches it.
	inline void SetErase(uintptr_t a_p)
	{
		size_t h = (a_p >> 4) * 0x9E3779B97F4A7C15ull >> 46;
		for (size_t i = 0; i < 64; ++i) {
			auto&     slot = g_shapeSet[(h + i) & (kSetSize - 1)];
			uintptr_t cur = a_p;
			if (slot.compare_exchange_strong(cur, 2)) return;
			if (cur == 0) return;
		}
	}

	inline bool SetContains(uintptr_t a_p)
	{
		size_t h = (a_p >> 4) * 0x9E3779B97F4A7C15ull >> 46;
		for (size_t i = 0; i < 64; ++i) {
			uintptr_t cur = g_shapeSet[(h + i) & (kSetSize - 1)].load(std::memory_order_relaxed);
			if (cur == a_p) return true;
			if (cur == 0) return false;
		}
		return false;
	}

	// ---- loading ---------------------------------------------------------------------------------------
	// gotham .bin: "AWG1", u32 hullCount, then per hull: u32 n, n * float3 (guest m, relative).
	inline bool LoadFile(const std::wstring& a_path, double a_x, double a_y, double a_z, int a_limit, float a_radius)
	{
		FILE* f = _wfopen(a_path.c_str(), L"rb");
		if (!f) {
			Log("gotham: can't open %ls", a_path.c_str());
			return false;
		}
		char     magic[4];
		uint32_t count = 0;
		if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "AWG1", 4) || fread(&count, 4, 1, f) != 1) {
			fclose(f);
			Log("gotham: bad file %ls", a_path.c_str());
			return false;
		}
		std::vector<Hull> hulls;
		std::vector<std::pair<float, uint32_t>> order;  // nearest first
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t n = 0;
			if (fread(&n, 4, 1, f) != 1 || n > 4096) break;
			Hull h;
			h.pts.resize(n * 3ull);
			if (fread(h.pts.data(), 4, n * 3ull, f) != n * 3ull) break;
			float cx = 0, cy = 0, cz = 0;
			for (uint32_t k = 0; k < n; ++k) cx += h.pts[k * 3], cy += h.pts[k * 3 + 1], cz += h.pts[k * 3 + 2];
			order.emplace_back(cx * cx + cy * cy + cz * cz, static_cast<uint32_t>(hulls.size()));
			hulls.push_back(std::move(h));
		}
		fclose(f);
		std::sort(order.begin(), order.end());
		g_pending.clear();
		for (size_t i = 0; i < order.size() && (a_limit <= 0 || static_cast<int>(i) < a_limit); ++i) g_pending.push_back(std::move(hulls[order[i].second]));
		g_nextPending = 0;
		g_place[0] = a_x, g_place[1] = a_y, g_place[2] = a_z;
		Log("gotham: %u hulls in file, %zu queued (nearest first), origin placed at %.2f %.2f %.2f", count, g_pending.size(), a_x, a_y, a_z);
		(void)a_radius;
		return true;
	}

	// ---- tiles: one hknpCompressedMeshShape body per tile (build_gotham_tiles.py) --------------------------
	// "AWT1", u32 tileCount; per tile: f32 center[3], u32 nv, u32 nt, nv * f32[3] (relative to center),
	// nt * u16[3]. Far fewer bodies than one per hull (the world holds about 32k bodies in total).
	struct Tile
	{
		float                 center[3];
		std::vector<float>    verts;
		std::vector<uint16_t> idx;
	};
	inline std::vector<Tile> g_tiles;
	inline bool              g_anchorValid = false;  // the hero is published in Arkham's frame (see LoadTiles)
	inline double            g_anchorGuest[3] = {};
	inline std::vector<Tile> g_keptTiles;
	inline size_t            g_nextTile = 0;
	inline uint16_t          g_tileShapeTag = 0xFFFF;  // per-triangle shape tag (0xFFFF = none)
	// collisionFilterInfo of every Gotham body (havok.h: the surface categories); `gotham surface <hex>`
	inline std::atomic<uint32_t> g_filterInfo{ hk::kGothamFilterInfo };
	inline std::vector<uint8_t*> g_staticSystems;  // systems of `gotham load` / `gotham tiles` (never removed)
	inline std::atomic<int>  g_tilesBuilt{ 0 }, g_tileFailures{ 0 }, g_tileTris{ 0 };

	// Parses AWT1 bytes; returns the number of tiles the header announced (0 = unreadable).
	inline uint32_t ParseTiles(const uint8_t* a_p, size_t a_n, std::vector<Tile>& a_out, size_t& a_tris)
	{
		a_tris = 0;
		size_t off = 0;
		auto   take = [&](void* a_dst, size_t a_bytes) {
			if (off + a_bytes > a_n) return false;
			memcpy(a_dst, a_p + off, a_bytes);
			off += a_bytes;
			return true;
		};
		char     magic[4];
		uint32_t count = 0;
		if (!take(magic, 4) || memcmp(magic, "AWT1", 4) || !take(&count, 4)) return 0;
		for (uint32_t i = 0; i < count; ++i) {
			Tile     t;
			uint32_t nv = 0, nt = 0;
			if (!take(t.center, 12) || !take(&nv, 4) || !take(&nt, 4) || nv > 65535 || nt > 1000000) break;
			t.verts.resize(nv * 3ull);
			t.idx.resize(nt * 3ull);
			if (!take(t.verts.data(), t.verts.size() * 4) || !take(t.idx.data(), t.idx.size() * 2)) break;
			a_tris += nt;
			a_out.push_back(std::move(t));
		}
		return count;
	}

	// Reads an AWT1 file (same result as ParseTiles).
	inline uint32_t ReadTiles(const std::wstring& a_path, std::vector<Tile>& a_out, size_t& a_tris)
	{
		a_tris = 0;
		FILE* f = _wfopen(a_path.c_str(), L"rb");
		if (!f) {
			Log("gotham: can't open %ls", a_path.c_str());
			return 0;
		}
		std::vector<uint8_t> bytes;
		uint8_t              buf[65536];
		for (size_t got; (got = fread(buf, 1, sizeof(buf), f)) > 0;) bytes.insert(bytes.end(), buf, buf + got);
		fclose(f);
		uint32_t count = ParseTiles(bytes.data(), bytes.size(), a_out, a_tris);
		if (!count) Log("gotham: bad tile file %ls", a_path.c_str());
		return count;
	}

	inline bool LoadTiles(const std::wstring& a_path, double a_x, double a_y, double a_z)
	{
		std::vector<Tile> tiles;
		size_t            tris = 0;
		uint32_t          count = ReadTiles(a_path, tiles, tris);
		if (!count) return false;
		g_tiles = std::move(tiles);
		g_nextTile = 0;
		g_place[0] = a_x, g_place[1] = a_y, g_place[2] = a_z;
		// <name>.origin: where the file's origin is in Arkham (feet, UU). With it the guest publishes the
		// hero in Arkham's frame: (hero - placement) + origin, so the host puts Batman on the same spot.
		std::wstring originPath = a_path.substr(0, a_path.find_last_of(L'.')) + L".origin";
		if (FILE* o = _wfopen(originPath.c_str(), L"r")) {
			double uu[3];
			if (fscanf(o, "%lf %lf %lf", &uu[0], &uu[1], &uu[2]) == 3) {
				coords::V3 gpos = coords::HostPosToGuest({ uu[0], uu[1], uu[2] });
				g_anchorGuest[0] = gpos.x, g_anchorGuest[1] = gpos.y, g_anchorGuest[2] = gpos.z;
				g_anchorValid = true;
				Log("gotham: anchored to Arkham feet (%.0f %.0f %.0f) UU = guest (%.2f %.2f %.2f)", uu[0], uu[1], uu[2], gpos.x, gpos.y, gpos.z);
			}
			fclose(o);
		}
		Log("gotham: %zu of %u tiles loaded (%zu triangles), origin placed at %.2f %.2f %.2f", g_tiles.size(), count, tris, a_x, a_y, a_z);
		return !g_tiles.empty();
	}

	inline void* BuildTileShape(const Tile& a_t)
	{
		alignas(16) uint8_t cinfo[hk::kCmsCinfoSize] = {};
		hk::Fn<hk::CtorFn>(hk::kCmsCinfoCtor)(cinfo);
		*reinterpret_cast<uintptr_t*>(cinfo) = g_exe + hk::kGameCmsCinfoVtable;
		*reinterpret_cast<const float**>(cinfo + 0x40) = a_t.verts.data();
		*reinterpret_cast<int32_t*>(cinfo + 0x48) = static_cast<int32_t>(a_t.verts.size() / 3);
		*reinterpret_cast<const uint16_t**>(cinfo + 0x50) = a_t.idx.data();
		*reinterpret_cast<int32_t*>(cinfo + 0x58) = static_cast<int32_t>(a_t.idx.size() / 3);
		*reinterpret_cast<uint64_t*>(cinfo + 0x60) = 0;  // no convex pieces
		*reinterpret_cast<uint16_t*>(cinfo + 0x68) = g_tileShapeTag;
		void* mem = hk::HavokAlloc(static_cast<int>(hk::kCmsSize));
		if (!mem) return nullptr;
		*reinterpret_cast<void**>(cinfo + 0x90) = mem;  // as exe+18116d0 stores its target there
		return hk::Fn<hk::CmsCtorFn>(hk::kCmsCtor)(mem, cinfo);
	}

	// ---- building (main thread) ---------------------------------------------------------------------------
	inline void* BuildShape(const Hull& a_h, float a_center[3])
	{
		size_t n = a_h.pts.size() / 3;
		if (n < 4) return nullptr;
		double c[3] = {};
		for (size_t k = 0; k < n; ++k) {
			for (int i = 0; i < 3; ++i) c[i] += a_h.pts[k * 3 + i];
		}
		for (int i = 0; i < 3; ++i) c[i] /= static_cast<double>(n);
		std::vector<float> v4(n * 4);
		for (size_t k = 0; k < n; ++k) {
			for (int i = 0; i < 3; ++i) v4[k * 4 + i] = static_cast<float>(a_h.pts[k * 3 + i] - c[i]);
			v4[k * 4 + 3] = 0.0f;
		}
		alignas(16) uint8_t cfg[0x80] = {};
		hk::Fn<hk::CtorFn>(hk::kBuildConfigCtor)(cfg);
		cfg[0x11] = 0;  // as Havok's own caller (exe+2e94e1a)
		cfg[0x2d] = 0;
		hk::StridedVertices sv{ v4.data(), static_cast<int32_t>(n), 16 };
		void* shape = hk::Fn<hk::CreateConvexFn>(hk::kCreateConvexFromVertices)(&sv, 0.0f, cfg);
		for (int i = 0; i < 3; ++i) a_center[i] = static_cast<float>(c[i] + g_place[i]);
		return shape;
	}

	// Turns the accumulated shapes into one hknpPhysicsSystem and adds it to the world.
	inline uint8_t* AddSystem(const Batch& b);

	inline bool FlushBatch()
	{
		if (g_building.shapes.empty()) return true;
		uint8_t* system = AddSystem(g_building);
		g_building = {};
		if (system) g_staticSystems.push_back(system);
		return system != nullptr;
	}

	inline uint8_t* AddSystem(const Batch& b)
	{
		int n = static_cast<int>(b.shapes.size());
		if (!n) return nullptr;
		void* world = hk::World();
		const void* motion = world ? hk::StaticMotionProperties(world) : nullptr;
		if (!world || !motion) {
			Log("gotham: no world / motion properties - batch dropped");
			return nullptr;
		}
		// Allocations live forever (Havok sees the DONT_DEALLOCATE flag on every array).
		auto* material = static_cast<uint8_t*>(_aligned_malloc(hk::kMaterialSize, 16));
		auto* motionProps = static_cast<uint8_t*>(_aligned_malloc(hk::kMotionPropsSize, 16));
		auto* cinfos = static_cast<uint8_t*>(_aligned_malloc(hk::kBodyCinfoSize * n, 16));
		auto* refs = static_cast<void**>(_aligned_malloc(sizeof(void*) * n, 16));
		auto* data = static_cast<uint8_t*>(_aligned_malloc(hk::kSystemDataSize, 16));
		auto* system = static_cast<uint8_t*>(_aligned_malloc(hk::kPhysicsSystemSize + 0x30, 16));
		for (void* p : { static_cast<void*>(material), static_cast<void*>(motionProps), static_cast<void*>(cinfos), static_cast<void*>(refs),
				 static_cast<void*>(data), static_cast<void*>(system) }) {
			g_keepAlive.push_back(p);
		}

		hk::Fn<hk::CtorFn>(hk::kMaterialCtor)(material);  // like CreateTile's TerrainDummyMaterial (unnamed)
		*reinterpret_cast<uint32_t*>(material + 0x28) = 0;
		material[0x38] = 2;
		material[0x39] = 2;
		*reinterpret_cast<float*>(material + 0x3C) = ReadOr<float>(g_exe + hk::kMaterialFloat, 0.0f);
		memcpy(motionProps, motion, hk::kMotionPropsSize);

		const float    cinfo70 = ReadOr<float>(g_exe + hk::kCinfoFloat70, 0.0f);
		const uint32_t filterInfo = g_filterInfo.load(std::memory_order_relaxed);
		for (int i = 0; i < n; ++i) {
			uint8_t* ci = cinfos + hk::kBodyCinfoSize * i;
			hk::Fn<hk::CtorFn>(hk::kBodyCinfoCtor)(ci);
			*reinterpret_cast<void**>(ci + 0x00) = b.shapes[i];
			*reinterpret_cast<uint32_t*>(ci + 0x10) = filterInfo;
			*reinterpret_cast<uint16_t*>(ci + 0x14) = 0;  // material index
			ci[0x28] = 0;
			float* pos = reinterpret_cast<float*>(ci + 0x30);
			pos[0] = b.centers[i * 3], pos[1] = b.centers[i * 3 + 1], pos[2] = b.centers[i * 3 + 2], pos[3] = 0.0f;
			float* q = reinterpret_cast<float*>(ci + 0x40);
			q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
			*reinterpret_cast<float*>(ci + 0x70) = cinfo70;
			*reinterpret_cast<int32_t*>(ci + 0xA0) = -1;
			refs[i] = b.shapes[i];
			hk::Fn<hk::AddRefFn>(hk::kAddReference)(b.shapes[i]);
		}

		memset(data, 0, hk::kSystemDataSize);
		*reinterpret_cast<uintptr_t*>(data) = g_exe + hk::kSystemDataVtable;
		*reinterpret_cast<uint64_t*>(data + 0x10) = 0x1ffff;
		*reinterpret_cast<uint16_t*>(data + 0x10) = 0xffff;
		auto arr = [&](int a_off, void* a_p, int a_n) {
			auto* a = reinterpret_cast<hk::HkArray*>(data + a_off);
			a->data = a_p;
			a->size = a_n;
			a->capacityAndFlags = 0x80000000u | static_cast<uint32_t>(a_n);
		};
		arr(0x18, material, 1);
		arr(0x28, motionProps, 1);
		arr(0x38, cinfos, n);
		arr(0x48, nullptr, 0);
		arr(0x58, refs, n);
		data[0x70] = 1;

		memset(system, 0, hk::kPhysicsSystemSize);
		CRITICAL_SECTION* cs = hk::ZoneLock();
		if (cs) EnterCriticalSection(cs);
		hk::Fn<hk::SystemCtorFn>(hk::kPhysicsSystemCtor)(system, world, data, reinterpret_cast<const void*>(g_exe + hk::kIdentityTransform), 3);
		hk::Fn<hk::SystemAddFn>(hk::kPhysicsSystemAdd)(system, 1, 0);
		if (cs) LeaveCriticalSection(cs);

		int ids = ReadOr<int32_t>(reinterpret_cast<uintptr_t>(system) + hk::kSystemBodyCount, -1);
		g_bodiesAdded += n;
		++g_systems;
		Log("gotham: system %d added: %d bodies (system reports %d body ids), lock %s", g_systems.load(), n, ids, cs ? "held" : "NOT initialized");
		return system;
	}

	// ---- streaming (tools/gotham_stream.py) ------------------------------------------------------------
	// The streamer scans Gotham in 50 m tiles around the hero and sends them through the collision ring
	// (kRecTile) or as files:
	//   gotham stream begin <x y z> [sky]  Arkham feet (UU) that the hero's spot stands for; with sky > 0
	//                                      Gotham goes that many meters ABOVE the hero, clear of New York's
	//                                      ledges and hints, and `gotham stream jump` puts him up there
	//   gotham stream load <key> <file>    AWT1 parts with centers in ABSOLUTE guest space (Arkham / 100)
	//   gotham stream unload <key>         removes that tile
	// Each tile is one hknpPhysicsSystem (one body per part). Unloading destroys its bodies the way the
	// system's own destructor does (exe+2e40450 -> hknpWorld::destroyBodies exe+2e50ae0); the shapes are
	// released two seconds later (no query still running can be inside them then), which frees their
	// Havok heap. Spider-Man's Havok heap has a fixed size: tiles that were never freed exhausted it and
	// the next compressed mesh build crashed (2026-10-01, exe+3710eb5), so no part is built while the
	// heap is low.
	struct StreamTile
	{
		std::vector<Tile>  parts;
		size_t             next = 0;  // next part to build
		Batch              built;
		std::vector<void*> shapes;  // all of the tile's shapes
		uint8_t*           system = nullptr;
		int                tris = 0;
		std::string        replaces;  // the older version of this tile, unloaded once this one is in
	};
	inline std::map<std::string, StreamTile> g_stream;       // pump thread only
	inline std::deque<std::string>           g_streamQueue;  // keys waiting to be built, in order
	inline bool                              g_streamOn = false;
	inline double                            g_sky = 0.0;    // meters Gotham sits above the hero's begin spot
	inline std::atomic<int>                  g_streamLoaded{ 0 }, g_streamUnloaded{ 0 }, g_streamBodiesDestroyed{ 0 }, g_shapesReleased{ 0 };
	inline std::atomic<int>                  g_buildsRefused{ 0 };
	inline std::atomic<int>                  g_tilesHeld{ 0 }, g_tilesQueued{ 0 };  // published to the streamer (worker thread)

	inline void UpdateCounts()
	{
		g_tilesHeld.store(static_cast<int>(g_stream.size()), std::memory_order_relaxed);
		g_tilesQueued.store(static_cast<int>(g_streamQueue.size()), std::memory_order_relaxed);
	}

	// Shapes of unloaded tiles, released once nothing can be using them any more.
	struct Retired
	{
		std::vector<void*> shapes;
		ULONGLONG          at;
	};
	inline std::vector<Retired> g_graveyard;
	inline bool                 g_releaseShapes = true;  // gotham stream release on|off

	// ---- Havok heap ------------------------------------------------------------------------------------
	// Havok's heap (hkMemoryRouter +0x58, the allocator every Havok container uses) has a fixed size in
	// Spider-Man. Any hkMemoryAllocator reports itself through getMemoryStatistics (vtable slot 8,
	// +0x40) into hkMemoryAllocator::MemoryStatistics: allocated, inUse, peakInUse, available,
	// totalAvailable, largestBlock (hkLong each, -1 = unlimited); hkFreeListAllocator's (exe+2b7a590)
	// takes its own lock. It needs the calling thread's router, so it is read on the pump thread.
	struct HeapStats
	{
		bool     valid = false;
		uint64_t used = 0, free = 0, largest = 0;
	};
	struct HkMemoryStatistics
	{
		int64_t allocated, inUse, peakInUse, available, totalAvailable, largestBlock;
	};
	inline uint64_t              g_minFreeBytes = 48ull << 20;
	inline std::atomic<bool>     g_heapGuard{ true };
	inline std::atomic<uint64_t> g_heapFree{ ~0ull }, g_heapUsed{ 0 };  // last pump-thread reading (~0: unknown)

	// The router's heap is the thread's hkThreadMemory (a cache of small blocks: its statistics are all
	// "unlimited"); the real heap is its parent at +0x8 (hkThreadMemory::blockAlloc exe+2bc4f80 hands
	// big blocks to [this+8]).
	inline uintptr_t HavokHeapAllocator()
	{
		uint32_t  tls = ReadOr<uint32_t>(g_exe + hk::kHkMemRouterTls, 0xFFFFFFFF);
		uintptr_t router = tls != 0xFFFFFFFF ? reinterpret_cast<uintptr_t>(TlsGetValue(tls)) : 0;
		if (!router) router = ReadOr<uintptr_t>(g_exe + hk::kHkMemRouterFallback, 0);
		uintptr_t a = router ? ReadOr<uintptr_t>(router + 0x58, 0) : 0;
		if (a && ReadOr<uintptr_t>(a, 0) == g_exe + hk::kHkThreadMemoryVtable) a = ReadOr<uintptr_t>(a + 0x8, 0);
		return a;
	}

	inline bool CallGetStats(uintptr_t a_alloc, HkMemoryStatistics& a_out)
	{
		using GetStatsFn = void (*)(void*, HkMemoryStatistics*);
		uintptr_t fn = ReadOr<uintptr_t>(ReadOr<uintptr_t>(a_alloc, 0) + 0x40, 0);
		if (!fn || !InModule(fn, g_exe)) return false;
		a_out = {};
		__try {
			reinterpret_cast<GetStatsFn>(fn)(reinterpret_cast<void*>(a_alloc), &a_out);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Pump thread only (it has a Havok memory router).
	inline HeapStats ReadHeap(HkMemoryStatistics* a_raw = nullptr)
	{
		HeapStats          s;
		HkMemoryStatistics m;
		uintptr_t          a = HavokHeapAllocator();
		if (!a || !CallGetStats(a, m)) return s;
		if (a_raw) *a_raw = m;
		int64_t freeBytes = m.totalAvailable >= 0 ? m.totalAvailable : m.available;
		if (freeBytes < 0 || m.allocated <= 0) return s;  // unlimited, or no real numbers: nothing to guard
		s.valid = true;
		s.used = static_cast<uint64_t>(std::max<int64_t>(m.inUse, 0));
		s.free = static_cast<uint64_t>(freeBytes);
		s.largest = static_cast<uint64_t>(std::max<int64_t>(m.largestBlock, 0));
		return s;
	}

	inline void SampleHeap()
	{
		static ULONGLONG s_last = 0;
		ULONGLONG        now = GetTickCount64();
		if (now - s_last < 250) return;
		s_last = now;
		HeapStats h = ReadHeap();
		g_heapFree.store(h.valid ? h.free : ~0ull, std::memory_order_relaxed);
		g_heapUsed.store(h.valid ? h.used : 0, std::memory_order_relaxed);
	}

	inline std::string RttiName(uintptr_t a_obj);

	// Spider-Man's physics world only reaches +-PhysicsBroadphaseRadius (PhysicsSettings, loaded from the
	// game's data into exe+7a88e8c: x, y, z; the world's broadphase box is -r..r, exe+18244b0). A body
	// outside it leaves the broadphase (Physics::OnLeftTheBroadPhaseCallBack). Gotham in the sky, its
	// tallest buildings (~240 m above the streets in the scans) and Spider-Man's highest swings must stay
	// inside, so the sky is lowered when the box isn't tall enough.
	constexpr uintptr_t kBroadphaseRadius = 0x7a88e8c;
	constexpr double    kSkyHeadroom = 450.0;

	inline void StreamBegin(const double a_originUU[3], const double a_hero[3], double a_sky)
	{
		float r[3] = {};
		SafeRead(r, reinterpret_cast<const void*>(g_exe + kBroadphaseRadius), sizeof(r));
		Log("gotham stream: Spider-Man's physics world reaches +-%.0f / %.0f / %.0f m (x / y / z); hero at y %.1f", r[0], r[1], r[2], a_hero[1]);
		if (a_sky > 0.0 && r[1] > 0.0f) {
			double room = r[1] - a_hero[1] - kSkyHeadroom;
			if (a_sky > room) {
				Log("gotham stream: %.0f m up would put Gotham's tops outside the physics world - %.0f m instead", a_sky, std::max(room, 0.0));
				a_sky = std::max(room, 0.0);
			}
		} else if (a_sky > 0.0) {
			Log("gotham stream: physics world size unknown - %.0f m up unchecked", a_sky);
		}
		coords::V3 g = coords::HostPosToGuest({ a_originUU[0], a_originUU[1], a_originUU[2] });
		g_anchorGuest[0] = g.x, g_anchorGuest[1] = g.y, g_anchorGuest[2] = g.z;
		for (int i = 0; i < 3; ++i) g_place[i] = a_hero[i];
		g_place[1] += a_sky;
		g_sky = a_sky;
		g_anchorValid = a_sky <= 0.0;  // in the sky: only once the hero has jumped up
		g_streamOn = true;
		HkMemoryStatistics m{};
		HeapStats          h = ReadHeap(&m);
		uintptr_t          alloc = HavokHeapAllocator();
		Log("gotham stream: Arkham feet (%.0f %.0f %.0f) UU = guest (%.2f %.2f %.2f) placed at (%.2f %.2f %.2f)%s", a_originUU[0], a_originUU[1],
			a_originUU[2], g.x, g.y, g.z, g_place[0], g_place[1], g_place[2], a_sky > 0 ? " in the sky" : " at the hero");
		Log("gotham stream: Havok heap %p %s: allocated %.1f MB, in use %.1f MB, peak %.1f MB, available %.1f MB, total available %.1f MB, largest block %.1f MB%s",
			reinterpret_cast<void*>(alloc), alloc ? RttiName(alloc).c_str() : "-", m.allocated / 1048576.0, m.inUse / 1048576.0, m.peakInUse / 1048576.0,
			m.available / 1048576.0, m.totalAvailable / 1048576.0, m.largestBlock / 1048576.0, h.valid ? "" : " - NOT USABLE, no heap guard");
	}

	// Absolute guest position (Arkham / 100) -> where it is in New York's world.
	inline float Placed(float a_abs, int a_axis) { return static_cast<float>(a_abs - g_anchorGuest[a_axis] + g_place[a_axis]); }

	inline void DestroySystemBodies(uint8_t* a_system)
	{
		auto     base = reinterpret_cast<uintptr_t>(a_system);
		auto*    ids = reinterpret_cast<uint32_t*>(ReadOr<uintptr_t>(base + hk::kSystemBodyIds, 0));
		int      n = ReadOr<int32_t>(base + hk::kSystemBodyCount, 0);
		void*    world = hk::World();
		if (!ids || n <= 0 || n > 4096 || !world) return;
		std::vector<uint32_t> live;
		for (int i = 0; i < n; ++i) {
			if ((ids[i] & 0xffffff) != 0xffffff) live.push_back(ids[i]);
		}
		CRITICAL_SECTION* cs = hk::ZoneLock();
		if (cs) EnterCriticalSection(cs);
		if (!live.empty()) hk::Fn<hk::DestroyBodiesFn>(hk::kWorldDestroyBodies)(world, live.data(), static_cast<int>(live.size()), 0);
		*reinterpret_cast<int32_t*>(base + hk::kSystemBodyCount) = 0;  // nothing left for anyone to destroy twice
		if (cs) LeaveCriticalSection(cs);
		g_streamBodiesDestroyed += static_cast<int>(live.size());
	}

	// Rewrites the collisionFilterInfo of a system's live bodies: the query filter and the hit info read
	// it from the body at query time, so new surface categories apply at once.
	inline int RetagSystem(uint8_t* a_system, uint32_t a_info)
	{
		void*     world = hk::World();
		uintptr_t bodies = world ? ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(world) + hk::kWorldBodies, 0) : 0;
		auto      base = reinterpret_cast<uintptr_t>(a_system);
		uintptr_t ids = ReadOr<uintptr_t>(base + hk::kSystemBodyIds, 0);
		int       n = ReadOr<int32_t>(base + hk::kSystemBodyCount, 0);
		if (!bodies || !ids || n <= 0 || n > 4096) return 0;
		int done = 0;
		for (int i = 0; i < n; ++i) {
			uint32_t id = ReadOr<uint32_t>(ids + i * 4ull, 0xffffffff) & 0xffffff;
			if (id == 0xffffff) continue;
			uintptr_t body = bodies + id * hk::kBodySize;
			if (!SetContains(ReadOr<uintptr_t>(body + hk::kBodyShape, 0))) continue;  // not (or no longer) one of ours
			if (SafeWrite(reinterpret_cast<void*>(body + hk::kBodyFilterInfo), &a_info, 4)) ++done;
		}
		return done;
	}

	// `gotham surface <hex>`: the filter info for new Gotham bodies, and for all the live ones. Pump thread.
	inline int SetFilterInfo(uint32_t a_info)
	{
		g_filterInfo = a_info;
		int n = 0;
		for (uint8_t* s : g_staticSystems) n += RetagSystem(s, a_info);
		for (auto& kv : g_stream) {
			if (kv.second.system) n += RetagSystem(kv.second.system, a_info);
		}
		return n;
	}

	// A shape we built has two references of ours (its own from the constructor, the one AddSystem adds
	// for the system data); a body holds a third until it is destroyed. Dropping ours deletes it.
	inline void ReleaseShape(void* a_shape)
	{
		auto remove = hk::Fn<hk::RemoveRefFn>(hk::kRemoveReference);
		if (g_shapesReleased < 3) {  // the first few: show the reference count (2 = only ours left, it gets deleted)
			uint32_t m = ReadOr<uint32_t>(reinterpret_cast<uintptr_t>(a_shape) + 0x10, 0);
			Log("gotham stream: releasing shape %p, %u references, memory size field %04x", a_shape, m >> 16, m & 0xFFFF);
		}
		remove(a_shape);
		remove(a_shape);
		SetErase(reinterpret_cast<uintptr_t>(a_shape) | 1);  // its address may come back as another shape
		++g_shapesReleased;
	}

	// a_withOld: also the retired version a reload of this tile keeps until the new one is in
	inline void StreamUnload(const std::string& a_key, bool a_withOld = true)
	{
		static const std::string kOld = "~old";
		if (a_withOld && (a_key.size() < kOld.size() || a_key.compare(a_key.size() - kOld.size(), kOld.size(), kOld) != 0)) StreamUnload(a_key + kOld);
		auto it = g_stream.find(a_key);
		if (it == g_stream.end()) return;
		StreamTile& t = it->second;
		for (void* s : t.shapes) SetMarkDead(reinterpret_cast<uintptr_t>(s));  // no query sees it from now on
		if (t.system) {
			DestroySystemBodies(t.system);
			if (g_releaseShapes) g_graveyard.push_back({ std::move(t.shapes), GetTickCount64() });
		} else if (g_releaseShapes) {
			// built parts of a tile that never got its system: in no body, only our first reference
			for (void* s : t.shapes) hk::Fn<hk::RemoveRefFn>(hk::kRemoveReference)(s), SetErase(reinterpret_cast<uintptr_t>(s) | 1);
		}
		g_streamQueue.erase(std::remove(g_streamQueue.begin(), g_streamQueue.end(), a_key), g_streamQueue.end());
		g_stream.erase(it);
		++g_streamUnloaded;
		UpdateCounts();
	}

	inline bool StreamLoadParts(const std::string& a_key, std::vector<Tile>&& a_parts, size_t a_tris)
	{
		if (!g_streamOn) {
			Log("gotham stream: load before begin - ignored");
			return false;
		}
		// A reload (better scan, new PhysX buildings) replaces the old version only once the new one is
		// in the world: the hero may be standing on it.
		StreamTile t;
		auto       old = g_stream.find(a_key);
		if (old != g_stream.end() && old->second.system) {
			std::string retired = a_key + "~old";
			StreamUnload(retired);  // an even older one still waiting
			g_stream[retired] = std::move(old->second);
			g_stream.erase(a_key);
			t.replaces = retired;
		} else {
			// a version still queued (not built) goes, but the built one it was to replace stays in the world
			// until this one is in: dropping it with the queued one left the tile without collision while
			// the new one was built
			if (old != g_stream.end()) t.replaces = std::move(old->second.replaces);
			StreamUnload(a_key, false);
		}
		t.parts = std::move(a_parts);
		t.tris = static_cast<int>(a_tris);
		g_stream[a_key] = std::move(t);
		g_streamQueue.push_back(a_key);
		UpdateCounts();
		return true;
	}

	inline bool StreamLoad(const std::string& a_key, const std::wstring& a_path)
	{
		std::vector<Tile> parts;
		size_t            tris = 0;
		if (!ReadTiles(a_path, parts, tris)) return false;
		return StreamLoadParts(a_key, std::move(parts), tris);
	}

	inline bool StreamLoadMem(const std::string& a_key, const uint8_t* a_awt, size_t a_n)
	{
		std::vector<Tile> parts;
		size_t            tris = 0;
		if (!ParseTiles(a_awt, a_n, parts, tris)) {
			Log("gotham stream: tile %s from the ring is unreadable (%zu bytes)", a_key.c_str(), a_n);
			return false;
		}
		return StreamLoadParts(a_key, std::move(parts), tris);
	}

	// Where the hero is in New York's world (the worker writes it): tiles are built nearest first.
	inline std::atomic<double> g_heroX{ 0.0 }, g_heroZ{ 0.0 };
	inline std::atomic<bool>   g_heroKnown{ false };

	inline double TileDistance(const StreamTile& a_t)
	{
		if (!g_heroKnown.load(std::memory_order_relaxed)) return 0.0;
		const float* c = a_t.parts.empty() ? nullptr : a_t.parts.front().center;
		if (!c) return 1e9;
		return std::hypot(Placed(c[0], 0) - g_heroX.load(std::memory_order_relaxed), Placed(c[2], 2) - g_heroZ.load(std::memory_order_relaxed));
	}

	// The queued tile nearest the hero goes first (the one he is about to reach can't wait behind far
	// rebuilds); returns its distance.
	inline double PickNearest()
	{
		size_t best = 0;
		double bestD = 1e18;
		for (size_t i = 0; i < g_streamQueue.size(); ++i) {
			auto it = g_stream.find(g_streamQueue[i]);
			double d = it == g_stream.end() ? -1.0 : TileDistance(it->second);  // stale entries first: they are dropped at once
			if (d < bestD) bestD = d, best = i;
		}
		if (best) std::swap(g_streamQueue[0], g_streamQueue[best]);
		return bestD;
	}

	// Build cost: each part is one compressed-mesh build inside Spider-Man's physics step, and his camera
	// drives Arkham's view, so a long one is a hitch on screen. Logged every 10 s; `gotham stream buildms`.
	inline std::atomic<double> g_buildMsPerStep{ 4.0 };  // a second part in the same step only while under this
	inline int                 g_bsParts = 0, g_bsTris = 0, g_bsSystems = 0;
	inline double              g_bsMs = 0.0, g_bsWorst = 0.0, g_bsSystemMs = 0.0;
	inline ULONGLONG           g_bsSince = 0;

	inline double QpcMs(const LARGE_INTEGER& a_from)
	{
		static double s_freq = 0.0;
		if (s_freq == 0.0) {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			s_freq = static_cast<double>(f.QuadPart);
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return (now.QuadPart - a_from.QuadPart) * 1000.0 / s_freq;
	}

	inline void LogBuildStats()
	{
		ULONGLONG now = GetTickCount64();
		if (!g_bsSince) g_bsSince = now;
		if (now - g_bsSince < 10000) return;
		if (g_bsParts || g_bsSystems) {
			Log("gotham stream: %d parts built (%d triangles) in %.0f ms, worst %.1f ms; %d tiles added in %.0f ms", g_bsParts, g_bsTris, g_bsMs, g_bsWorst,
				g_bsSystems, g_bsSystemMs);
		}
		g_bsParts = g_bsTris = g_bsSystems = 0;
		g_bsMs = g_bsWorst = g_bsSystemMs = 0.0;
		g_bsSince = now;
	}

	// Parts are built nearest tile first: up to three per physics step for a tile within 70 m of the hero
	// while the step's build time is under g_buildMsPerStep, else one. A tile's system is added once all its
	// parts are built. Retired shapes are released here too, two seconds after their tile went.
	inline bool StreamPump()
	{
		LogBuildStats();
		LARGE_INTEGER stepStart;
		QueryPerformanceCounter(&stepStart);
		if (!g_graveyard.empty() && GetTickCount64() - g_graveyard.front().at > 2000) {
			for (void* s : g_graveyard.front().shapes) ReleaseShape(s);
			g_graveyard.erase(g_graveyard.begin());
		}
		int budget = 0;
		while (!g_streamQueue.empty()) {
			if (!budget) budget = PickNearest() < 70.0 ? 3 : 1;
			auto it = g_stream.find(g_streamQueue.front());
			if (it == g_stream.end()) {
				g_streamQueue.pop_front();
				continue;
			}
			StreamTile& t = it->second;
			if (t.next < t.parts.size()) {
				if (g_heapGuard.load(std::memory_order_relaxed)) {
					HeapStats h = ReadHeap();  // fresh: the last build may have taken a lot
					if (h.valid && h.free < g_minFreeBytes) {
						static ULONGLONG s_logged = 0;
						++g_buildsRefused;
						if (GetTickCount64() - s_logged > 5000) {
							s_logged = GetTickCount64();
							Log("gotham stream: Havok heap low (%.0f MB free, largest block %.0f MB) - %s waits", h.free / 1048576.0, h.largest / 1048576.0,
								it->first.c_str());
						}
						return false;  // the streamer sees the low heap and unloads far tiles
					}
				}
				Tile&         p = t.parts[t.next++];
				LARGE_INTEGER b0;
				QueryPerformanceCounter(&b0);
				void*  shape = BuildTileShape(p);
				double ms = QpcMs(b0);
				++g_bsParts;
				g_bsTris += static_cast<int>(p.idx.size() / 3);
				g_bsMs += ms;
				g_bsWorst = std::max(g_bsWorst, ms);
				if (shape) {
					SetInsert(reinterpret_cast<uintptr_t>(shape));
					t.built.shapes.push_back(shape);
					t.shapes.push_back(shape);
					for (int i = 0; i < 3; ++i) t.built.centers.push_back(Placed(p.center[i], i));
				} else {
					++g_tileFailures;
					Log("gotham stream: %s part %zu (%zu tris) failed", it->first.c_str(), t.next - 1, p.idx.size() / 3);
				}
				// the same tile, while it is urgent and the step has time left
				if (--budget > 0 && t.next < t.parts.size() && QpcMs(stepStart) < g_buildMsPerStep.load(std::memory_order_relaxed)) continue;
				return true;
			}
			LARGE_INTEGER a0;
			QueryPerformanceCounter(&a0);
			t.system = AddSystem(t.built);
			++g_bsSystems;
			g_bsSystemMs += QpcMs(a0);
			t.built = {};  // the source arrays stay until unload (process memory, not the Havok heap)
			++g_streamLoaded;
			g_streamQueue.pop_front();
			if (!t.replaces.empty()) {
				std::string old = std::move(t.replaces);  // t may move when the map changes
				StreamUnload(old);
			}
			UpdateCounts();
			return true;
		}
		return false;
	}

	// Called at the start of every frame on the main thread.
	inline void Pump()
	{
		if (g_streamOn) SampleHeap();
		if (StreamPump()) return;
		// one tile per physics step (a compressed mesh build is heavy), bodies flushed at the end
		if (g_nextTile < g_tiles.size()) {
			Tile& t = g_tiles[g_nextTile++];
			void* shape = BuildTileShape(t);
			if (!shape) {
				++g_tileFailures;
				Log("gotham: tile %zu (%zu tris) failed", g_nextTile - 1, t.idx.size() / 3);
			} else {
				++g_tilesBuilt;
				g_tileTris += static_cast<int>(t.idx.size() / 3);
				SetInsert(reinterpret_cast<uintptr_t>(shape));
				g_building.shapes.push_back(shape);
				float c[3] = { static_cast<float>(t.center[0] + g_place[0]), static_cast<float>(t.center[1] + g_place[1]),
					static_cast<float>(t.center[2] + g_place[2]) };
				g_building.centers.insert(g_building.centers.end(), c, c + 3);
			}
			g_keptTiles.push_back(std::move(t));  // kept in case the shape still points at the source arrays
			if (g_nextTile == g_tiles.size()) {
				FlushBatch();
				Log("gotham: tiles done - %d built (%d triangles), %d failed, %d bodies in %d systems", g_tilesBuilt.load(), g_tileTris.load(),
					g_tileFailures.load(), g_bodiesAdded.load(), g_systems.load());
				g_tiles.clear();
				g_nextTile = 0;
			}
			return;
		}
		int budget = g_perFrame;
		while (budget-- > 0 && g_nextPending < g_pending.size()) {
			float c[3];
			void* shape = BuildShape(g_pending[g_nextPending++], c);
			if (!shape) {
				++g_shapeFailures;
				continue;
			}
			++g_shapesBuilt;
			SetInsert(reinterpret_cast<uintptr_t>(shape));
			g_building.shapes.push_back(shape);
			g_building.centers.insert(g_building.centers.end(), c, c + 3);
			if (static_cast<int>(g_building.shapes.size()) >= g_batch) FlushBatch();
		}
		if (g_nextPending >= g_pending.size() && !g_building.shapes.empty()) FlushBatch();
		if (g_nextPending && g_nextPending == g_pending.size()) {
			Log("gotham: done - %d shapes built, %d failed, %d bodies in %d systems", g_shapesBuilt.load(), g_shapeFailures.load(),
				g_bodiesAdded.load(), g_systems.load());
			g_pending.clear();
			g_nextPending = 0;
		}
	}

	// ---- query filter --------------------------------------------------------------------------------------
	using BodyFilterFn = bool (*)(void* a_filter, int a_queryType, const void* a_filterData, const void* a_body);
	inline BodyFilterFn      g_origBodyFilter = nullptr;
	inline std::atomic<bool> g_filterOn{ false };
	inline std::atomic<bool> g_hidden{ false };  // with the filter off: Gotham bodies rejected instead (gotham hide/show)
	inline std::atomic<int>  g_shapeOffset{ -1 };  // offset of the shape pointer in hknpBody, found at runtime
	inline std::atomic<uint64_t> g_filterCalls{ 0 }, g_filterRejected{ 0 };
	inline uintptr_t         g_patchedVtable = 0;

	inline bool IsGothamBody(const void* a_body)
	{
		auto b = reinterpret_cast<uintptr_t>(a_body);
		int  off = g_shapeOffset.load(std::memory_order_relaxed);
		if (off >= 0) return SetContains(ReadOr<uintptr_t>(b + off, 0));
		for (int o = 0; o < 0x100; o += 8) {  // calibration: find which field holds one of our shapes
			uintptr_t p = ReadOr<uintptr_t>(b + o, 0);
			if (p && SetContains(p)) {
				g_shapeOffset = o;
				Log("gotham: hknpBody shape pointer is at +0x%x", o);
				return true;
			}
		}
		return false;
	}

	inline std::string RttiName(uintptr_t a_obj);

	inline std::string RttiOf(uintptr_t a_p)
	{
		return a_p > 0x10000 && a_p < 0x7FFFFFFF0000ull && !(a_p & 7) ? RttiName(a_p) : std::string();
	}

	// ---- query stats: which entry point (none/ray/shape/closest) reaches which filter slot, per query type
	enum Entry { kEntryNone, kEntryRay, kEntryShape, kEntryClosest, kEntryCount };
	inline thread_local int g_entry = kEntryNone;
	inline std::atomic<uint64_t> g_entryCalls[kEntryCount] = {};
	inline std::atomic<uint64_t> g_slotCalls[2][kEntryCount][16] = {};  // [slot 6, slot 4][entry][queryType & 15]

	inline void CountSlot(int a_slot, int a_queryType)
	{
		g_slotCalls[a_slot][g_entry][a_queryType & 15].fetch_add(1, std::memory_order_relaxed);
	}

	inline void LogQueryStats()
	{
		static const char* entries[] = { "none", "ray", "shape", "closest" };
		for (int e = 1; e < kEntryCount; ++e) Log("  %s queries: %llu", entries[e], static_cast<unsigned long long>(g_entryCalls[e].exchange(0)));
		for (int s = 0; s < 2; ++s)
			for (int e = 0; e < kEntryCount; ++e)
				for (int t = 0; t < 16; ++t) {
					uint64_t n = g_slotCalls[s][e][t].exchange(0);
					if (n) Log("  slot %d from %s, query type %d: %llu", s ? 4 : 6, entries[e], t, static_cast<unsigned long long>(n));
				}
	}

	inline bool BodyFilterDetour(void* a_filter, int a_queryType, const void* a_filterData, const void* a_body)
	{
		CountSlot(0, a_queryType);
		g_filterCalls.fetch_add(1, std::memory_order_relaxed);
		if (g_deadCount.load(std::memory_order_relaxed) > 0) {
			int off = g_shapeOffset.load(std::memory_order_relaxed);
			if (off >= 0 && SetContains(ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(a_body) + off, 0) | 1)) return false;  // an unloaded tile
		}
		if (g_shapeSetCount.load(std::memory_order_relaxed) > 0) {
			if (g_filterOn.load(std::memory_order_relaxed)) {
				bool ours = IsGothamBody(a_body);
				if (!ours && g_shapeOffset.load(std::memory_order_relaxed) >= 0) {
					g_filterRejected.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
			} else if (g_hidden.load(std::memory_order_relaxed) && IsGothamBody(a_body)) {
				return false;  // "gotham hide": our bodies are invisible to every query
			}
		}
		return g_origBodyFilter(a_filter, a_queryType, a_filterData, a_body);
	}

	// Slot 4: isCollisionEnabled(queryType, bool targetShapeIsB, const FilterInput& a, const FilterInput& b),
	// the shape-level check (exe+18076d0 only compares the filter infos at input +4). FilterInput, read
	// from live samples 2026-10-01: +0x04 collisionFilterInfo, +0x10 hknpBody*, +0x18 root shape,
	// +0x20 parent shape, +0x28 shape key, +0x30 leaf shape (often a temporary on the stack). The world
	// side is b when the flag is set (Havok's naming). A Gotham body's root shape is our convex shape.
	using ShapeFilterFn = bool (*)(void* a_filter, int a_queryType, bool a_targetIsB, const void* a_a, const void* a_b);
	constexpr int                kInputRootShape = 0x18;
	inline ShapeFilterFn         g_origShapeFilter = nullptr;
	inline std::atomic<bool>     g_slot4On{ true };  // "gotham slot4 off" falls back to the body-level check only
	inline std::atomic<bool>     g_slot4Dumped{ false };
	inline std::atomic<uint64_t> g_shapeRejected[16] = {};  // by query type

	inline void DumpInputs(int a_queryType, bool a_flag, const void* a_a, const void* a_b)
	{
		uintptr_t a[8] = {}, b[8] = {};
		SafeRead(a, a_a, sizeof(a));
		SafeRead(b, a_b, sizeof(b));
		Log("gotham: slot 4 sample: type %d flag %d", a_queryType, a_flag);
		for (int i = 0; i < 8; ++i)
			Log("   +%02x  a %016llx %s  b %016llx %s", i * 8, static_cast<unsigned long long>(a[i]), RttiOf(a[i]).c_str(),
				static_cast<unsigned long long>(b[i]), RttiOf(b[i]).c_str());
	}

	inline bool ShapeFilterDetour(void* a_filter, int a_queryType, bool a_targetIsB, const void* a_a, const void* a_b)
	{
		CountSlot(1, a_queryType);
		if (g_filterOn.load(std::memory_order_relaxed) && g_slot4On.load(std::memory_order_relaxed) &&
			g_shapeSetCount.load(std::memory_order_relaxed) > 0) {
			auto      target = reinterpret_cast<uintptr_t>(a_targetIsB ? a_b : a_a);
			uintptr_t root = ReadOr<uintptr_t>(target + kInputRootShape, 0);
			if (!g_slot4Dumped.exchange(true)) DumpInputs(a_queryType, a_targetIsB, a_a, a_b);
			// only judge inputs that carry a root shape; anything else goes to the game's own check
			if (root && !SetContains(root)) {
				g_shapeRejected[a_queryType & 15].fetch_add(1, std::memory_order_relaxed);
				return false;
			}
		}
		return g_origShapeFilter(a_filter, a_queryType, a_targetIsB, a_a, a_b);
	}

	// MSVC RTTI class name of a polymorphic object in the exe (".?AVhknp...@@"), or "" if it isn't one.
	inline std::string RttiName(uintptr_t a_obj)
	{
		uintptr_t vt = ReadOr<uintptr_t>(a_obj, 0);
		if (!vt || !InModule(vt, g_exe)) return {};
		uintptr_t col = ReadOr<uintptr_t>(vt - 8, 0);
		if (!col || !InModule(col, g_exe)) return {};
		uint32_t td = ReadOr<uint32_t>(col + 12, 0);
		if (!td || !InModule(g_exe + td + 16, g_exe)) return {};
		char name[128] = {};
		SafeRead(name, reinterpret_cast<void*>(g_exe + td + 16), sizeof(name) - 1);
		return name;
	}

	// Finds the hknpCollisionFilter a world query carries (checked by RTTI class name, 2026-10-01:
	// query+0x00 is the shape tag codec, NOT the filter) and patches slot 6 of its class vtable once.
	inline void PatchFilterFrom(const void* a_query)
	{
		if (g_patchedVtable) return;
		auto q = reinterpret_cast<uintptr_t>(a_query);
		for (int off = 0; off <= 0x20; off += 8) {
			uintptr_t   obj = ReadOr<uintptr_t>(q + off, 0);
			std::string name = obj ? RttiName(obj) : std::string();
			Log("gotham: query+0x%02x = %p  %s", off, reinterpret_cast<void*>(obj), name.empty() ? "(not an RTTI object)" : name.c_str());
			if (name.find("CollisionFilter") == std::string::npos) continue;
			uintptr_t vt = ReadOr<uintptr_t>(obj, 0);
			bool      ok = true;
			for (int i = 0; i < 8 && ok; ++i) {
				uintptr_t fn = ReadOr<uintptr_t>(vt + i * 8, 0);
				ok = InModule(fn, g_exe);
			}
			if (!ok) {
				Log("gotham: %s vtable has fewer than 8 valid slots - not patched", name.c_str());
				continue;
			}
			auto* slot = reinterpret_cast<uintptr_t*>(vt + 4 * 8);  // slots 4..6 are adjacent
			g_origShapeFilter = reinterpret_cast<ShapeFilterFn>(slot[0]);
			g_origBodyFilter = reinterpret_cast<BodyFilterFn>(slot[2]);
			DWORD prot;
			VirtualProtect(slot, 24, PAGE_READWRITE, &prot);
			slot[0] = reinterpret_cast<uintptr_t>(&ShapeFilterDetour);
			slot[2] = reinterpret_cast<uintptr_t>(&BodyFilterDetour);
			VirtualProtect(slot, 24, prot, &prot);
			g_patchedVtable = vt;
			Log("gotham: query filter %s at query+0x%x, vtable exe+%llx slots 4+6 patched (orig exe+%llx, exe+%llx)", name.c_str(), off,
				static_cast<unsigned long long>(vt - g_exe), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_origShapeFilter) - g_exe),
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_origBodyFilter) - g_exe));
			return;
		}
		g_patchedVtable = 1;  // nothing suitable: don't look again (logged above)
		Log("gotham: no CollisionFilter found in the query - New York filter unavailable");
	}

	// ---- `gotham hitflags [s]`: the surface categories that come back with hits ----------------------------
	// For a few seconds every finished ray / shape cast with the game's hit collector is looked at: per
	// (body filter info, hit category bits, Gotham or not) the number of hits. In New York (Gotham hidden)
	// it shows what the city's own surfaces carry; on Gotham, what ours do.
	inline std::atomic<ULONGLONG>          g_hitSampleUntil{ 0 };
	inline SRWLOCK                         g_hitLock = SRWLOCK_INIT;
	inline std::map<uint64_t, uint64_t>    g_hitHist;  // (ours << 63) | (body info << 16) | flags -> hits
	inline std::atomic<uint64_t>           g_hitQueries{ 0 };

	inline void SampleHits(const void* a_collector)
	{
		if (GetTickCount64() > g_hitSampleUntil.load(std::memory_order_relaxed)) return;
		auto c = reinterpret_cast<uintptr_t>(a_collector);
		if (!c || ReadOr<uintptr_t>(c, 0) != g_exe + hk::kHitCollectorVtable) return;
		int       n = ReadOr<int32_t>(c + 0xc, 0);
		uintptr_t recs = ReadOr<uintptr_t>(c + 0x38, 0);
		void*     world = hk::World();
		uintptr_t bodies = world ? ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(world) + hk::kWorldBodies, 0) : 0;
		if (n <= 0 || n > 256 || !recs || !bodies) return;
		g_hitQueries.fetch_add(1, std::memory_order_relaxed);
		uint64_t keys[16];
		int      k = 0;
		for (int i = 0; i < n && k < 16; ++i) {
			uintptr_t r = recs + i * 0x90ull;
			uint32_t  id = ReadOr<uint32_t>(r + 0x48, 0xffffffff) & 0xffffff;
			if (id == 0xffffff) continue;
			uintptr_t body = bodies + id * hk::kBodySize;
			uint32_t  info = ReadOr<uint32_t>(body + hk::kBodyFilterInfo, 0);
			uint32_t  flags = ReadOr<uint32_t>(r + 0x78, 0) & hk::kCategoryMask;
			bool      ours = SetContains(ReadOr<uintptr_t>(body + hk::kBodyShape, 0));
			keys[k++] = (static_cast<uint64_t>(ours) << 63) | (static_cast<uint64_t>(info) << 16) | flags;
		}
		AcquireSRWLockExclusive(&g_hitLock);
		for (int i = 0; i < k; ++i) ++g_hitHist[keys[i]];
		ReleaseSRWLockExclusive(&g_hitLock);
	}

	inline void LogHitFlags()
	{
		std::vector<std::pair<uint64_t, uint64_t>> v;
		AcquireSRWLockExclusive(&g_hitLock);
		for (auto& kv : g_hitHist) v.push_back({ kv.second, kv.first });
		g_hitHist.clear();
		ReleaseSRWLockExclusive(&g_hitLock);
		std::sort(v.rbegin(), v.rend());
		Log("gotham hitflags: %llu queries looked at, %zu kinds of hit", static_cast<unsigned long long>(g_hitQueries.exchange(0)), v.size());
		for (size_t i = 0; i < v.size() && i < 24; ++i) {
			uint64_t key = v[i].second;
			Log("  %8llu hits  %s body info %08x  hit categories %03x", static_cast<unsigned long long>(v[i].first), (key >> 63) ? "GOTHAM  " : "new york",
				static_cast<uint32_t>((key >> 16) & 0xffffffff), static_cast<uint32_t>(key & 0x7ff));
		}
	}

	// `gotham presets`: the query presets' category masks (which surfaces each kind of probe sees).
	inline void LogPresets()
	{
		uintptr_t table = ReadOr<uintptr_t>(g_exe + hk::kQueryPresets, 0);
		uint32_t  n = ReadOr<uint32_t>(ReadOr<uintptr_t>(g_exe + hk::kQueryPresetCount, 0) + 4, 0);
		Log("gotham presets: table %p, %u presets; Gotham bodies carry %08x", reinterpret_cast<void*>(table), n, g_filterInfo.load());
		for (uint32_t i = 0; table && i < n && i < 64; ++i) {
			uintptr_t e = table + i * 32ull;
			uint16_t  mask = ReadOr<uint16_t>(e + 6, 0), layers = ReadOr<uint16_t>(e + 8, 0);
			uint32_t  w = ReadOr<uint32_t>(e + 0xc, 0);
			bool      sees = !(mask & hk::kCategoryMask) || (mask & g_filterInfo.load() & hk::kCategoryMask);
			Log("  preset %2u: categories %03x layers %04x word %08x%s", i, mask, layers, w, sees ? "" : "   <- does NOT see Gotham");
		}
	}

	inline void Init()
	{
		g_shapeSet = new std::atomic<uintptr_t>[kSetSize]();
	}
}
