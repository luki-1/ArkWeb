// Arkham's grapple ledges for Spider-Man's zip to point (sm_guest/zip.h).
//
// Every grapple target Batman can use is a ledge segment: RGrapplePoint actors (GrappleInfo +0x348)
// and the GrapplePointInfo arrays of RGrapplePointCollection actors (TArray +0x29c), each with two
// points on the edge (+0xc, +0x18), the outward normal of the wall under it (+0x0), a type (+0x48)
// and flags (+0x4c: low priority 1, swingable 2, valid 8). They stream in and out with Gotham's levels.
//
// A slice of GObjects is looked at every game tick, on the game thread: an access violation is fatal
// in Arkham even inside __try (its crash handler sees it first), and only there are the objects
// guaranteed alive while they are read. After a full pass the ledges go to the worker, which writes
// logs\stream\grapple.awg when they changed (written to .part, then renamed):
//   "AWG1", u32 count, per ledge f32 a[3], b[3], n[3] (UU), u32 flags
//   (type | low priority << 8 | swingable << 9 | from a collection << 10 | valid << 11)
#pragma once

#include "../common/util.h"
#include "ue3.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace arkweb::grapple
{
	constexpr int kPointInfo = 0x348;       // RGrapplePoint.GrappleInfo (GrapplePointInfo)
	constexpr int kPointBits = 0x29c;       // RGrapplePoint bitfield: bUnusableGrapplePoint 0x8
	constexpr int kCollectionInfo = 0x29c;  // RGrapplePointCollection.GrappleInfo (TArray<GrapplePointInfo>)
	constexpr int kInfoBytes = 0x50;

	struct Edge
	{
		float    a[3], b[3], n[3];
		uint32_t flags;
	};
	static_assert(sizeof(Edge) == 40);

	inline uintptr_t         g_cls[3] = {};  // RGrapplePoint, RRescuePoint, RGrapplePointCollection
	inline std::atomic<bool> g_classesReady{ false };
	inline std::atomic<bool> g_enabled{ true };
	inline std::atomic<int>  g_perTick{ 3000 };  // GObjects slots per tick (`grapple pertick`)

	// game thread
	inline int g_cursor = 0;
	struct Found
	{
		int       index;  // its GObjects slot: still holding this object = still alive
		uintptr_t obj;
	};
	inline std::vector<Found> g_found;
	inline uint64_t               g_lastHash = 0;
	inline int                    g_passes = 0;

	// game thread -> worker
	inline SRWLOCK           g_lock = SRWLOCK_INIT;
	inline std::vector<Edge> g_out;
	inline bool              g_dirty = false;
	inline std::atomic<int>  g_edges{ 0 }, g_actors{ 0 }, g_collections{ 0 }, g_files{ 0 };

	inline bool UserPtr(uintptr_t a_p) { return a_p > 0x10000 && a_p < 0x7FFFFFFF0000ull && !(a_p & 3); }

	inline bool Finite(const float* a_v, int a_n)
	{
		for (int i = 0; i < a_n; ++i) {
			if (!std::isfinite(a_v[i]) || std::fabs(a_v[i]) > 1.0e6f) return false;
		}
		return true;
	}

	inline bool ReadInfo(uintptr_t a_info, bool a_collection, Edge& a_out)
	{
		uint8_t raw[kInfoBytes];
		memcpy(raw, reinterpret_cast<const void*>(a_info), sizeof(raw));
		float n[3], a[3], b[3];
		memcpy(n, raw + 0x0, 12);
		memcpy(a, raw + 0xc, 12);
		memcpy(b, raw + 0x18, 12);
		if (!Finite(n, 3) || !Finite(a, 3) || !Finite(b, 3)) return false;
		if (a[0] == 0.0f && a[1] == 0.0f && a[2] == 0.0f && b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f) return false;  // a class default
		float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (nl > 0.1f) {
			for (float& v : n) v /= nl;
		} else {
			n[0] = n[1] = 0.0f, n[2] = 1.0f;
		}
		uint8_t  type = raw[0x48];
		uint32_t bits;
		memcpy(&bits, raw + 0x4c, 4);
		memcpy(a_out.a, a, 12);
		memcpy(a_out.b, b, 12);
		memcpy(a_out.n, n, 12);
		a_out.flags = type | ((bits & 1) << 8) | ((bits & 2) << 8) | (a_collection ? 1u << 10 : 0u) | ((bits & 8) << 8);
		return true;
	}

	inline uintptr_t SlotAt(int a_i)
	{
		uintptr_t o;
		memcpy(&o, reinterpret_cast<const uint8_t*>(g_exe + ue3::kGObjData) + static_cast<size_t>(a_i) * 8, 8);
		return o;
	}

	// Game thread: the ledges of the actors one full pass found. A pass takes a second or two, so an actor
	// seen early may have streamed out since: only those whose GObjects slot still holds them are read
	// (a destroyed object's slot is cleared before its memory goes back).
	inline void Collect(const std::vector<Found>& a_actors)
	{
		std::vector<Edge>              edges;
		std::unordered_set<uint64_t>   seen;
		int                            points = 0, collections = 0;
		auto add = [&](const Edge& e) {
			// the same ledge twice (a collection and a point, a level loaded twice): keep one
			int64_t  qx = std::lround((e.a[0] + e.b[0]) * 0.05f), qy = std::lround((e.a[1] + e.b[1]) * 0.05f), qz = std::lround((e.a[2] + e.b[2]) * 0.05f);
			uint64_t key = (static_cast<uint64_t>(qx) * 73856093ull) ^ (static_cast<uint64_t>(qy) * 19349663ull) ^ (static_cast<uint64_t>(qz) * 83492791ull);
			if (seen.insert(key).second) edges.push_back(e);
		};
		int n = ue3::ObjCount();
		for (const Found& f : a_actors) {
			if (f.index >= n || SlotAt(f.index) != f.obj) continue;
			uintptr_t o = f.obj;
			uintptr_t cls = 0;
			memcpy(&cls, reinterpret_cast<const void*>(o + ue3::kObjClass), 8);
			if (cls && cls == g_cls[2]) {
				ue3::TArrayRaw arr;
				memcpy(&arr, reinterpret_cast<const void*>(o + kCollectionInfo), sizeof(arr));
				if (arr.num <= 0 || arr.num > 20000 || arr.num > arr.max || !UserPtr(arr.data)) continue;
				++collections;
				for (int k = 0; k < arr.num && edges.size() < 50000; ++k) {
					Edge e;
					if (ReadInfo(arr.data + static_cast<uintptr_t>(k) * kInfoBytes, true, e)) add(e);
				}
			} else if (cls && (cls == g_cls[0] || cls == g_cls[1])) {
				uint32_t bits = 0;
				memcpy(&bits, reinterpret_cast<const void*>(o + kPointBits), 4);
				if (bits & 0x8) continue;  // bUnusableGrapplePoint
				++points;
				Edge e;
				if (ReadInfo(o + kPointInfo, false, e)) add(e);
			}
		}
		uint64_t h = 1469598103934665603ull;
		for (const Edge& e : edges) {
			const auto* p = reinterpret_cast<const uint8_t*>(&e);
			for (size_t i = 0; i < sizeof(e); ++i) h = (h ^ p[i]) * 1099511628211ull;
		}
		g_actors = points;
		g_collections = collections;
		g_edges = static_cast<int>(edges.size());
		if (++g_passes <= 2) Log("grapple: pass %d: %d grapple points, %d collections, %zu ledges", g_passes, points, collections, edges.size());
		if (h == g_lastHash) return;
		g_lastHash = h;
		AcquireSRWLockExclusive(&g_lock);
		g_out = std::move(edges);
		g_dirty = true;
		ReleaseSRWLockExclusive(&g_lock);
	}

	// Game thread, every tick.
	inline void Tick()
	{
		if (!g_enabled.load(std::memory_order_relaxed) || !g_classesReady.load(std::memory_order_acquire)) return;
		int n = ue3::ObjCount();
		int end = std::min(n, g_cursor + std::max(500, g_perTick.load(std::memory_order_relaxed)));
		for (int i = g_cursor; i < end; ++i) {
			uintptr_t o = SlotAt(i);
			if (!UserPtr(o)) continue;
			uintptr_t cls;
			memcpy(&cls, reinterpret_cast<const void*>(o + ue3::kObjClass), 8);
			if (cls && (cls == g_cls[0] || cls == g_cls[1] || cls == g_cls[2])) g_found.push_back({ i, o });
		}
		g_cursor = end;
		if (g_cursor < n) return;
		g_cursor = 0;
		Collect(g_found);
		g_found.clear();
	}

	// Worker thread: writes the file when the game thread handed over new ledges.
	inline void WriteIfDirty(const std::wstring& a_dir)
	{
		std::vector<Edge> edges;
		AcquireSRWLockExclusive(&g_lock);
		bool dirty = g_dirty;
		if (dirty) edges.swap(g_out), g_dirty = false;
		ReleaseSRWLockExclusive(&g_lock);
		if (!dirty) return;
		CreateDirectoryW(a_dir.c_str(), nullptr);
		std::wstring path = a_dir + L"\\grapple.awg";
		FILE*        f = _wfopen((path + L".part").c_str(), L"wb");
		if (!f) return;
		uint32_t count = static_cast<uint32_t>(edges.size());
		fwrite("AWG1", 1, 4, f);
		fwrite(&count, 4, 1, f);
		if (count) fwrite(edges.data(), sizeof(Edge), count, f);
		fclose(f);
		MoveFileExW((path + L".part").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (++g_files <= 3 || g_files % 50 == 0) {
			Log("grapple: %u ledges written for Spider-Man (file %d)", count, g_files.load());
			if (count && g_files <= 1) {
				const Edge& e = edges.front();
				Log("  first: (%.0f %.0f %.0f) - (%.0f %.0f %.0f) normal (%.2f %.2f %.2f) flags %x", e.a[0], e.a[1], e.a[2], e.b[0], e.b[1], e.b[2], e.n[0], e.n[1],
					e.n[2], e.flags);
			}
		}
	}

	inline void Status()
	{
		Log("  grapple: %s, classes %p %p %p, %d per tick; last pass %d points, %d collections, %d ledges; %d files written", g_enabled ? "on" : "off",
			reinterpret_cast<void*>(g_cls[0]), reinterpret_cast<void*>(g_cls[1]), reinterpret_cast<void*>(g_cls[2]), g_perTick.load(), g_actors.load(),
			g_collections.load(), g_edges.load(), g_files.load());
	}
}
