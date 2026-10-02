// Developer commands for the AK host: Claude writes logs\ak_cmd.txt while the game runs; the host
// runs the lines on the GAME THREAD (from PlayerTick, so PhysX/UE3 reads don't race the game),
// logs results to ak_host.log and deletes the file.
//
//   mem <addr> <hexsize>            hex + float dump      (addr hex, or exe+<rva>, px+<rva>)
//   ptr <addr>                      read a qword
//   px info                         PhysX scenes, actor counts, static shape types, scale guess
//   px mesh <n>                     first n static triangle-mesh shapes: poses, geometry, raw mesh, layout
//   px convex <n>                   first n static convex shapes: geometry, raw mesh, hull-data layout
//   px export <radius_uu> <name>    static geometry within radius of Batman -> logs\<name>.obj (UU)
//   scan <radius_m> <step_m> <name> [filter]  Unreal collision (Actor.Trace down columns) -> logs\<name>.scan
//   tscan <name> <x0> <y0> <x1> <y1> <step> <top> <bottom> [filter]   (UU, absolute) one streamed tile ->
//                                   logs\stream\<name>.scan; queued, several may be sent at once
//   px bexport <radius_uu> <name>   like px export (horizontal radius), binary -> logs\stream\<name>.awp;
//                                   incremental: only shapes not sent before (px bforget starts over)
//   scanbudget <ms>                 scan time per game tick (default 4)
//   exportbudget <ms>               px bexport time per game tick (default 2; the export runs as a job)
//   overlay on|off|scale <f>|offset <u> <v>|tint <r> <g> <b>|status   Spider-Man's frame over Arkham's (overlay.h)
//   batman hide|show|auto           Batman's model (auto: hidden while Spider-Man is drawn)
//   shot [count] [every]            Arkham's finished frames as half-size BMPs -> logs\shots
//   grapple status|on|off|pertick <n>   grapple ledges for Spider-Man's zip to point (grapple.h)
#pragma once

#include "grapple.h"
#include "overlay.h"
#include "px.h"
#include "ue3.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arkweb::devcmd
{
	inline px::Api        g_px;
	inline px::MeshLayout g_meshLayout;
	inline px::ConvexLayout g_convexLayout;
	inline float          g_pxScale = 0.0f;  // PhysX units per UU (found by px info / export)

	// Collision scan (scan <radius_m> <step_m> <name>): the command only arms the job; main.cpp steps
	// it on the game thread with Actor.Trace (Unreal collision, which has the streets PhysX lacks).
	struct ScanJob
	{
		bool         active = false;
		ue3::FVector center{};
		float        radiusUU = 0, stepUU = 0;
		float        topUU = 20000, bottomUU = -30000;  // column start / end, relative to Batman's Z (absolute for a tile)
		int          n = 0, next = 0;  // grid is n x n columns, next = linear index
		uint8_t      filter = 66;      // ECollisionFilter: 0 NoCollision, 66 TraceWorld, 88 TraceSnapToFloor, 101 GrappleSwingTarget
		FILE*        out = nullptr;
		uint64_t     traces = 0, hits = 0;
		ULONGLONG    startMs = 0;
		// tile scans (tscan, from the streamer): an absolute rectangle, nx x ny columns from (x0, y0),
		// written to <part> and renamed to <final> when complete so the streamer never reads half a file
		bool         tile = false;
		float        x0 = 0, y0 = 0;
		int          nx = 0, ny = 0;
		std::wstring part, final;
		std::string  name;
	};
	inline ScanJob             g_scan;
	inline std::deque<ScanJob> g_scanQueue;  // tile scans waiting their turn
	inline double              g_scanBudgetMs = 4.0;  // scan time per game tick
	inline float               g_scanExtent = 10.0f;  // tile scans sweep a box of this half size (UU); scanextent <uu>

	// Single debug traces (trace dx dy dz ex ey ez [filter], meters relative to Batman), run by main.cpp
	// on the game thread with the full result logged.
	struct TraceReq
	{
		ue3::FVector start, end;
		uint8_t      filter;
	};
	inline TraceReq g_traceReqs[16];
	inline int      g_traceReqCount = 0;

	// tp <x> <y> <z>: puts Batman (feet, UU) there on the next tick - rescues a save whose checkpoint
	// ended up under the map while he was the puppet.
	inline bool         g_tpPending = false;
	inline ue3::FVector g_tpFeet{};

	// cam mimic on|off: Arkham shows Spider-Man's camera while Batman is the puppet (MimicCamera in the ini).
	inline bool g_mimicCamera = true;
	inline int  g_batmanHide = -1;  // 1 hidden, 0 shown, -1 hidden while the overlay draws Spider-Man
	inline bool g_mimicFov = true;  // the view's field of view is Spider-Man's too (`cam fov on|off`)

	inline uintptr_t ParseAddr(const char* a_s)
	{
		if (!a_s) return 0;
		if (!_strnicmp(a_s, "exe+", 4)) return g_exe + strtoull(a_s + 4, nullptr, 16);
		if (!_strnicmp(a_s, "px+", 3)) return reinterpret_cast<uintptr_t>(g_px.dll) + strtoull(a_s + 3, nullptr, 16);
		return strtoull(a_s, nullptr, 16);
	}

	inline void HexDump(uintptr_t a_addr, size_t a_n)
	{
		std::vector<uint8_t> b(a_n);
		if (!SafeRead(b.data(), reinterpret_cast<void*>(a_addr), a_n)) {
			Log("  %p: unreadable", reinterpret_cast<void*>(a_addr));
			return;
		}
		for (size_t o = 0; o < a_n; o += 16) {
			char line[200];
			int  p = snprintf(line, sizeof(line), "  +%04zx:", o);
			for (size_t i = 0; i < 16 && o + i < a_n; ++i) p += snprintf(line + p, sizeof(line) - p, " %02x", b[o + i]);
			p += snprintf(line + p, sizeof(line) - p, "  |");
			for (size_t i = 0; i + 4 <= 16 && o + i + 4 <= a_n; i += 4) {
				float f;
				memcpy(&f, &b[o + i], 4);
				p += snprintf(line + p, sizeof(line) - p, " %g", f);
			}
			Log("%s", line);
		}
	}

	inline std::string PxRva(uintptr_t a_p)
	{
		char b[64];
		uintptr_t base = reinterpret_cast<uintptr_t>(g_px.dll);
		if (a_p >= base && a_p < base + 0x400000) snprintf(b, sizeof(b), "px+%llx", static_cast<unsigned long long>(a_p - base));
		else snprintf(b, sizeof(b), "%p", reinterpret_cast<void*>(a_p));
		return b;
	}

	// All static actors of all scenes.
	inline std::vector<void*> StaticActors()
	{
		std::vector<void*> out;
		void* phys = g_px.getPhysics ? g_px.getPhysics() : nullptr;
		if (!phys) return out;
		void* scenes[16] = {};
		uint32_t ns = px::Scenes(phys, scenes, 16);
		for (uint32_t s = 0; s < ns; ++s) {
			uint32_t n = px::NbActors(scenes[s], px::kRigidStatic);
			size_t   base = out.size();
			out.resize(base + n);
			uint32_t got = px::Actors(scenes[s], px::kRigidStatic, out.data() + base, n, 0);
			out.resize(base + got);
		}
		return out;
	}

	inline void PxInfo(const ue3::FVector& a_batman)
	{
		void* phys = g_px.getPhysics();
		void* scenes[16] = {};
		uint32_t ns = px::Scenes(phys, scenes, 16);
		Log("PhysX: PxPhysics %p, %u scenes (getNbScenes %u)", phys, ns, px::NbScenes(phys));
		for (uint32_t s = 0; s < ns; ++s) {
			Log("  scene %u %p: static %u, dynamic %u", s, scenes[s], px::NbActors(scenes[s], px::kRigidStatic), px::NbActors(scenes[s], px::kRigidDynamic));
		}
		auto actors = StaticActors();
		std::map<int, int> types;
		int shapes = 0;
		// scale guess: which scale puts the most static actors within 60 m of Batman?
		const float cand[] = { 1.0f, 0.02f, 0.01f };
		int         within[3] = {};
		for (void* a : actors) {
			px::Transform t = px::GlobalPose(a);
			for (int c = 0; c < 3; ++c) {
				float dx = t.p.x - a_batman.x * cand[c], dy = t.p.y - a_batman.y * cand[c], dz = t.p.z - a_batman.z * cand[c];
				float r = 6000.0f * cand[c];
				if (dx * dx + dy * dy + dz * dz < r * r) ++within[c];
			}
			void* sh[8];
			uint32_t n = px::Shapes(a, sh, 8);
			for (uint32_t i = 0; i < n; ++i) types[px::GeometryTypeOf(sh[i])]++, ++shapes;
		}
		Log("  %zu static actors, %d shapes; types:", actors.size(), shapes);
		for (auto& [t, c] : types) Log("    type %d: %d", t, c);
		Log("  actors within 60 m of Batman for scale 1 / 0.02 / 0.01 (PhysX units per UU): %d / %d / %d", within[0], within[1], within[2]);
		int best = within[1] >= within[0] && within[1] >= within[2] ? 1 : within[2] > within[0] ? 2 : 0;
		g_pxScale = cand[best];
		Log("  using scale %g", g_pxScale);
		if (!actors.empty()) {
			px::Transform t = px::GlobalPose(actors[0]);
			Log("  first actor %p vtable %s pose q(%g %g %g %g) p(%g %g %g)", actors[0], PxRva(*reinterpret_cast<uintptr_t*>(actors[0])).c_str(),
				t.q.x, t.q.y, t.q.z, t.q.w, t.p.x, t.p.y, t.p.z);
		}
	}

	inline void PxMesh(int a_n)
	{
		auto actors = StaticActors();
		int  shown = 0;
		for (void* a : actors) {
			void* sh[8];
			uint32_t n = px::Shapes(a, sh, 8);
			for (uint32_t i = 0; i < n && shown < a_n; ++i) {
				if (px::GeometryTypeOf(sh[i]) != px::eTRIANGLEMESH) continue;
				px::TriangleMeshGeometry g{};
				bool ok = g_px.triGeom(&g, sh[i], g);
				px::Transform ap = px::GlobalPose(a), lp = px::LocalPose(sh[i]);
				uintptr_t mesh = reinterpret_cast<uintptr_t>(g.triangleMesh);
				Log("mesh shape %d: actor %p shape %p (vt %s) geom %s type %d scale (%g %g %g) mesh %p vt %s", shown, a, sh[i],
					PxRva(*reinterpret_cast<uintptr_t*>(sh[i])).c_str(), ok ? "ok" : "FAIL", g.type, g.scale.scale.x, g.scale.scale.y,
					g.scale.scale.z, g.triangleMesh, PxRva(ReadOr<uintptr_t>(mesh, 0)).c_str());
				Log("  actor pose p(%g %g %g) q(%g %g %g %g); local p(%g %g %g)", ap.p.x, ap.p.y, ap.p.z, ap.q.x, ap.q.y, ap.q.z, ap.q.w, lp.p.x, lp.p.y, lp.p.z);
				HexDump(mesh, 0x180);
				px::MeshLayout l = px::FindMeshLayout(mesh);
				Log("  layout: counts +0x%x, verts +0x%x, tris +0x%x%s", l.offNbVerts, l.offVerts, l.offTris, l.Valid() ? "" : " (NOT FOUND)");
				if (l.Valid() && !g_meshLayout.Valid()) g_meshLayout = l;
				++shown;
			}
			if (shown >= a_n) break;
		}
	}

	inline void PxConvex(int a_n)
	{
		auto actors = StaticActors();
		int  shown = 0;
		for (void* a : actors) {
			void* sh[8];
			uint32_t n = px::Shapes(a, sh, 8);
			for (uint32_t i = 0; i < n && shown < a_n; ++i) {
				if (px::GeometryTypeOf(sh[i]) != px::eCONVEXMESH) continue;
				px::ConvexMeshGeometry g{};
				bool ok = g_px.convexGeom && g_px.convexGeom(&g, sh[i], g);
				uintptr_t mesh = reinterpret_cast<uintptr_t>(g.convexMesh);
				px::Transform ap = px::GlobalPose(a);
				Log("convex %d: actor %p shape %p geom %s scale (%g %g %g) mesh %p vt %s, actor p(%g %g %g)", shown, a, sh[i], ok ? "ok" : "FAIL",
					g.scale.scale.x, g.scale.scale.y, g.scale.scale.z, g.convexMesh, PxRva(ReadOr<uintptr_t>(mesh, 0)).c_str(), ap.p.x, ap.p.y, ap.p.z);
				HexDump(mesh, 0x80);
				px::ConvexLayout l = px::FindConvexLayout(mesh);
				std::vector<px::Vec3> v;
				if (l.Valid()) px::ReadConvexAt(mesh, l.offHull, &v);
				Log("  hull data at +0x%x%s, %zu vertices%s", l.offHull, l.Valid() ? "" : " (NOT FOUND)", v.size(),
					v.empty() ? "" : "; first vertex:");
				if (!v.empty()) Log("    (%g %g %g)", v[0].x, v[0].y, v[0].z);
				if (l.Valid() && !g_convexLayout.Valid()) g_convexLayout = l;
				++shown;
			}
			if (shown >= a_n) break;
		}
	}

	inline std::unordered_map<void*, uint32_t> g_sentShapes;  // px bexport: shape -> pose stamp already sent (px bforget clears)
	inline std::unordered_set<void*>           g_doneActors;  // px bexport: actors whose shapes all went out
	inline uint32_t                            g_exportCount = 0;

	inline std::wstring StreamDir()
	{
		std::wstring d = g_logDir + L"\\stream";
		CreateDirectoryW(d.c_str(), nullptr);
		return d;
	}

	// a_binary (px bexport, for the streamer): "AWP1", f32 Batman xyz, f32 radius, u32 shape count, then per
	// shape u32 type (0 triangle mesh, 1 box, 2 convex points), u32 nv, u32 nt, nv * f32[3] UU, nt * u32[3];
	// shapes are kept by horizontal distance. Written to .part and renamed when complete.
	inline void PxExport(const ue3::FVector& a_batman, float a_radiusUU, const char* a_name, bool a_binary = false)
	{
		if (g_pxScale == 0.0f) PxInfo(a_batman);
		LARGE_INTEGER t0;
		QueryPerformanceCounter(&t0);
		size_t       already = 0;
		std::string  n = a_name;
		std::wstring wn(n.begin(), n.end());
		std::wstring path = a_binary ? StreamDir() + L"\\" + wn + L".awp" : g_logDir + L"\\" + wn + L".obj";
		FILE*        f = _wfopen(a_binary ? (path + L".part").c_str() : path.c_str(), a_binary ? L"wb" : L"w");
		if (!f) return;
		uint32_t shapes = 0;
		if (a_binary) {
			float hdr[4] = { a_batman.x, a_batman.y, a_batman.z, a_radiusUU };
			fwrite("AWP1", 1, 4, f);
			fwrite(hdr, 4, 4, f);
			fwrite(&shapes, 4, 1, f);  // patched at the end
		} else {
			fprintf(f, "# ArkWeb Gotham export, UU, Z up. Batman %.1f %.1f %.1f, radius %.0f, PhysX scale %g\n", a_batman.x, a_batman.y, a_batman.z,
				a_radiusUU, g_pxScale);
		}
		auto     actors = StaticActors();
		float    inv = 1.0f / g_pxScale, r2 = a_radiusUU * a_radiusUU;
		size_t   vbase = 1, tris = 0, meshes = 0, boxes = 0, convexes = 0, skipped = 0, failed = 0, bad = 0, knownActors = 0;
		px::MeshData md;
		// Incremental exports skip actors whose shapes all went out before - most of the ~40k, which
		// took ~80 ms a time (an Arkham hitch). Every 8th export checks them all again, in case an
		// address was reused by a newly streamed actor.
		bool full = a_binary && (g_exportCount++ % 8) == 0;
		if (full) g_doneActors.clear();
		for (void* a : actors) {
			if (a_binary && !full && g_doneActors.count(a)) {
				++knownActors;
				continue;
			}
			px::Transform ap = px::GlobalPose(a);
			void* sh[8];
			uint32_t ns = px::Shapes(a, sh, 8);
			size_t   sentBefore = shapes, alreadyBefore = already;
			for (uint32_t i = 0; i < ns; ++i) {
				px::Transform pose = px::Mul(ap, px::LocalPose(sh[i]));
				uint32_t      stamp = 0;
				if (a_binary) {  // incremental: a shape goes out once (a reused address at a new pose counts as new)
					float pp[3] = { pose.p.x, pose.p.y, pose.p.z };
					uint32_t b[3];
					memcpy(b, pp, sizeof(b));
					stamp = (b[0] * 0x9E3779B1u) ^ (b[1] * 0x85EBCA77u) ^ (b[2] * 0xC2B2AE3Du) | 1u;
					auto it = g_sentShapes.find(sh[i]);
					if (it != g_sentShapes.end() && it->second == stamp) {
						++already;
						continue;
					}
				}
				int type = px::GeometryTypeOf(sh[i]);
				std::vector<px::Vec3> v;
				std::vector<uint32_t> t;
				if (type == px::eTRIANGLEMESH) {
					px::TriangleMeshGeometry g{};
					if (!g_px.triGeom(&g, sh[i], g)) { ++failed; continue; }
					uintptr_t mesh = reinterpret_cast<uintptr_t>(g.triangleMesh);
					// Only two triangle meshes exist in Gotham and a shared layout guess proved unreliable:
					// detect it per mesh.
					if (!px::ReadMesh(mesh, px::FindMeshLayout(mesh), md)) { ++bad; continue; }
					v.reserve(md.verts.size());
					for (auto& p : md.verts) v.push_back(px::Apply(pose, px::ApplyMeshScale(g.scale, p)));
					t = md.tris;
					++meshes;
				} else if (type == px::eBOX) {
					px::BoxGeometry g{};
					if (!g_px.boxGeom(&g, sh[i], g)) { ++failed; continue; }
					for (int k = 0; k < 8; ++k) {
						px::Vec3 c{ (k & 1 ? 1 : -1) * g.halfExtents.x, (k & 2 ? 1 : -1) * g.halfExtents.y, (k & 4 ? 1 : -1) * g.halfExtents.z };
						v.push_back(px::Apply(pose, c));
					}
					static const uint32_t kBox[36] = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
					t.assign(kBox, kBox + 36);
					++boxes;
				} else if (type == px::eCONVEXMESH && g_px.convexGeom) {
					px::ConvexMeshGeometry g{};
					if (!g_px.convexGeom(&g, sh[i], g)) { ++failed; continue; }
					uintptr_t mesh = reinterpret_cast<uintptr_t>(g.convexMesh);
					if (!g_convexLayout.Valid()) g_convexLayout = px::FindConvexLayout(mesh);
					std::vector<px::Vec3> cv;
					if (!g_convexLayout.Valid() || !px::ReadConvexAt(mesh, g_convexLayout.offHull, &cv)) { ++bad; continue; }
					for (auto& p : cv) v.push_back(px::Apply(pose, px::ApplyMeshScale(g.scale, p)));
					++convexes;  // points only: the hull is rebuilt from them (offline viewer / Havok)
				} else {
					++skipped;
					continue;
				}
				// Keep the shape if any vertex is inside the radius (in UU; horizontal only for bexport).
				bool isNear = false;
				for (auto& p : v) {
					float dx = p.x * inv - a_batman.x, dy = p.y * inv - a_batman.y, dz = a_binary ? 0.0f : p.z * inv - a_batman.z;
					if (dx * dx + dy * dy + dz * dz < r2) { isNear = true; break; }
				}
				if (!isNear) continue;
				if (a_binary) {
					uint32_t head[3] = { type == px::eTRIANGLEMESH ? 0u : type == px::eBOX ? 1u : 2u, static_cast<uint32_t>(v.size()),
						static_cast<uint32_t>(t.size() / 3) };
					fwrite(head, 4, 3, f);
					for (auto& p : v) {
						float q[3] = { p.x * inv, p.y * inv, p.z * inv };
						fwrite(q, 4, 3, f);
					}
					if (!t.empty()) fwrite(t.data(), 4, head[2] * 3, f);
					++shapes;
					g_sentShapes[sh[i]] = stamp;
				} else {
					fprintf(f, "o s%p\n", sh[i]);
					for (auto& p : v) fprintf(f, "v %.1f %.1f %.1f\n", p.x * inv, p.y * inv, p.z * inv);
					for (size_t k = 0; k + 2 < t.size(); k += 3) fprintf(f, "f %zu %zu %zu\n", vbase + t[k], vbase + t[k + 1], vbase + t[k + 2]);
				}
				vbase += v.size();
				tris += t.size() / 3;
			}
			// every shape out (now or before): a known actor from now on
			if (a_binary && ns && (shapes - sentBefore) + (already - alreadyBefore) == ns) g_doneActors.insert(a);
		}
		if (a_binary) {
			fseek(f, 20, SEEK_SET);
			fwrite(&shapes, 4, 1, f);
		}
		fclose(f);
		if (a_binary) {
			MoveFileExW((path + L".part").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
			LARGE_INTEGER t1, freq;
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&freq);
			Log("px bexport %s: %u new shapes (%zu sent before) of %zu actors (%zu known, skipped%s) in %.1f ms", a_name, shapes, already, actors.size(),
				knownActors, full ? "; full check" : "", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
			return;
		}
		Log("px export %s: %zu vertices, %zu triangles near Batman (r %.0f UU); scanned meshes %zu, boxes %zu, convexes %zu, other types "
			"skipped %zu, geometry failures %zu, unreadable %zu", a_name, vbase - 1, tris, a_radiusUU, meshes, boxes, convexes, skipped, failed, bad);
	}

	// ---- px bexport as a job: a couple of ms of Arkham's frame per tick ------------------------------------
	// In one go an export took 5-130 ms of the game thread (whenever Batman entered a new district, and
	// every 8th export was a full pass): a visible hitch every few seconds while swinging. The job takes the
	// static actors of THIS tick each tick (an actor list kept across ticks could hold actors Arkham has
	// released since) and goes on from where it stopped; actors shifting in the list between ticks only
	// means one is looked at twice (its shapes are already sent) or left for the next export.
	struct BExportJob
	{
		bool         active = false;
		std::string  name;
		std::wstring path;
		FILE*        f = nullptr;
		ue3::FVector batman{};
		float        radius = 0.0f;
		bool         full = false;
		size_t       next = 0;
		uint32_t     shapes = 0;
		size_t       already = 0, known = 0, actors = 0, failed = 0;
		double       ms = 0.0;
		int          ticks = 0;
		px::MeshData md;
	};
	inline BExportJob g_bx;
	inline double     g_exportBudgetMs = 2.0;  // `exportbudget <ms>`

	inline void BExportStart(const ue3::FVector& a_batman, float a_radiusUU, const char* a_name)
	{
		if (g_bx.active) {
			Log("px bexport %s: %s still running - ignored", a_name, g_bx.name.c_str());
			return;
		}
		if (g_pxScale == 0.0f) PxInfo(a_batman);
		if (g_pxScale == 0.0f) return;
		BExportJob j;
		j.name = a_name;
		std::wstring wn(j.name.begin(), j.name.end());
		j.path = StreamDir() + L"\\" + wn + L".awp";
		j.f = _wfopen((j.path + L".part").c_str(), L"wb");
		if (!j.f) return;
		float hdr[4] = { a_batman.x, a_batman.y, a_batman.z, a_radiusUU };
		uint32_t zero = 0;
		fwrite("AWP1", 1, 4, j.f);
		fwrite(hdr, 4, 4, j.f);
		fwrite(&zero, 4, 1, j.f);  // the shape count, patched at the end
		j.batman = a_batman;
		j.radius = a_radiusUU;
		j.full = (g_exportCount++ % 8) == 0;  // every 8th looks at known actors again (an address reused by a new actor)
		if (j.full) g_doneActors.clear();
		j.active = true;
		g_bx = std::move(j);
	}

	// One actor's shapes (binary export), as PxExport does them.
	inline void BExportActor(BExportJob& a_j, void* a_actor)
	{
		if (!a_j.full && g_doneActors.count(a_actor)) {
			++a_j.known;
			return;
		}
		++a_j.actors;
		float         inv = 1.0f / g_pxScale, r2 = a_j.radius * a_j.radius;
		px::Transform ap = px::GlobalPose(a_actor);
		void*         sh[8];
		uint32_t      ns = px::Shapes(a_actor, sh, 8);
		size_t        sentBefore = a_j.shapes, alreadyBefore = a_j.already;
		for (uint32_t i = 0; i < ns; ++i) {
			px::Transform pose = px::Mul(ap, px::LocalPose(sh[i]));
			float         pp[3] = { pose.p.x, pose.p.y, pose.p.z };
			uint32_t      b[3];
			memcpy(b, pp, sizeof(b));
			uint32_t stamp = (b[0] * 0x9E3779B1u) ^ (b[1] * 0x85EBCA77u) ^ (b[2] * 0xC2B2AE3Du) | 1u;
			auto     it = g_sentShapes.find(sh[i]);
			if (it != g_sentShapes.end() && it->second == stamp) {
				++a_j.already;
				continue;
			}
			int                   type = px::GeometryTypeOf(sh[i]);
			std::vector<px::Vec3> v;
			std::vector<uint32_t> t;
			if (type == px::eTRIANGLEMESH) {
				px::TriangleMeshGeometry g{};
				if (!g_px.triGeom(&g, sh[i], g)) { ++a_j.failed; continue; }
				uintptr_t mesh = reinterpret_cast<uintptr_t>(g.triangleMesh);
				if (!px::ReadMesh(mesh, px::FindMeshLayout(mesh), a_j.md)) { ++a_j.failed; continue; }
				v.reserve(a_j.md.verts.size());
				for (auto& q : a_j.md.verts) v.push_back(px::Apply(pose, px::ApplyMeshScale(g.scale, q)));
				t = a_j.md.tris;
			} else if (type == px::eBOX) {
				px::BoxGeometry g{};
				if (!g_px.boxGeom(&g, sh[i], g)) { ++a_j.failed; continue; }
				for (int k = 0; k < 8; ++k) {
					px::Vec3 c{ (k & 1 ? 1 : -1) * g.halfExtents.x, (k & 2 ? 1 : -1) * g.halfExtents.y, (k & 4 ? 1 : -1) * g.halfExtents.z };
					v.push_back(px::Apply(pose, c));
				}
				static const uint32_t kBox[36] = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
				t.assign(kBox, kBox + 36);
			} else if (type == px::eCONVEXMESH && g_px.convexGeom) {
				px::ConvexMeshGeometry g{};
				if (!g_px.convexGeom(&g, sh[i], g)) { ++a_j.failed; continue; }
				uintptr_t mesh = reinterpret_cast<uintptr_t>(g.convexMesh);
				if (!g_convexLayout.Valid()) g_convexLayout = px::FindConvexLayout(mesh);
				std::vector<px::Vec3> cv;
				if (!g_convexLayout.Valid() || !px::ReadConvexAt(mesh, g_convexLayout.offHull, &cv)) { ++a_j.failed; continue; }
				for (auto& q : cv) v.push_back(px::Apply(pose, px::ApplyMeshScale(g.scale, q)));
			} else {
				continue;
			}
			bool isNear = false;  // any vertex inside the (horizontal) radius
			for (auto& q : v) {
				float dx = q.x * inv - a_j.batman.x, dy = q.y * inv - a_j.batman.y;
				if (dx * dx + dy * dy < r2) { isNear = true; break; }
			}
			if (!isNear) continue;
			uint32_t head[3] = { type == px::eTRIANGLEMESH ? 0u : type == px::eBOX ? 1u : 2u, static_cast<uint32_t>(v.size()), static_cast<uint32_t>(t.size() / 3) };
			fwrite(head, 4, 3, a_j.f);
			for (auto& q : v) {
				float o[3] = { q.x * inv, q.y * inv, q.z * inv };
				fwrite(o, 4, 3, a_j.f);
			}
			if (!t.empty()) fwrite(t.data(), 4, head[2] * 3, a_j.f);
			++a_j.shapes;
			g_sentShapes[sh[i]] = stamp;
		}
		if (ns && (a_j.shapes - sentBefore) + (a_j.already - alreadyBefore) == ns) g_doneActors.insert(a_actor);
	}

	// Game thread, every tick.
	inline void StepExport()
	{
		if (!g_bx.active) return;
		LARGE_INTEGER t0, f;
		QueryPerformanceCounter(&t0);
		QueryPerformanceFrequency(&f);
		auto elapsed = [&] {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return (now.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
		};
		std::vector<void*> actors = StaticActors();
		BExportJob&        j = g_bx;
		while (j.next < actors.size()) {
			BExportActor(j, actors[j.next++]);
			if ((j.next & 63) == 0 && elapsed() > g_exportBudgetMs) break;
		}
		j.ms += elapsed();
		++j.ticks;
		if (j.next < actors.size()) return;
		fseek(j.f, 20, SEEK_SET);
		fwrite(&j.shapes, 4, 1, j.f);
		fclose(j.f);
		MoveFileExW((j.path + L".part").c_str(), j.path.c_str(), MOVEFILE_REPLACE_EXISTING);
		Log("px bexport %s: %u new shapes (%zu sent before) of %zu actors (%zu known, skipped%s), %.1f ms over %d ticks", j.name.c_str(), j.shapes, j.already,
			actors.size(), j.known, j.full ? "; full check" : "", j.ms, j.ticks);
		j = BExportJob{};
	}

	struct LineCtx
	{
		char*        c;
		char*        a1;
		char*        a2;
		char*        a3;
		ue3::FVector batman;
	};

	inline void ExecLine(void* a_p)
	{
		auto& x = *static_cast<LineCtx*>(a_p);
		const char *c = x.c, *a1 = x.a1, *a2 = x.a2, *a3 = x.a3;
		const ue3::FVector& a_batman = x.batman;
		if (!_stricmp(c, "mem") && a1) {
			HexDump(ParseAddr(a1), a2 ? strtoull(a2, nullptr, 16) : 0x100);
		} else if (!_stricmp(c, "ptr") && a1) {
			Log("  [%s] = %p", a1, reinterpret_cast<void*>(ReadOr<uintptr_t>(ParseAddr(a1), 0)));
		} else if (!_stricmp(c, "px") && a1 && !g_px.getPhysics) {
			Log("  PhysX not loaded");
		} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "info")) {
			PxInfo(a_batman);
		} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "mesh")) {
			PxMesh(a2 ? atoi(a2) : 3);
		} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "convex")) {
			PxConvex(a2 ? atoi(a2) : 3);
		} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "export") && a3) {
			PxExport(a_batman, static_cast<float>(atof(a2)), a3);
		} else {
			Log("  unknown command");
		}
	}

	// A crash inside a dev command (bad pointer, wrong vtable slot) is caught and logged instead of
	// taking the game down.
	inline bool Guarded(void (*a_fn)(void*), void* a_ctx, DWORD& a_code)
	{
		__try {
			a_fn(a_ctx);
			return true;
		} __except (a_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Runs one command line on the game thread (from ak_cmd.txt or the streamer's host ring).
	inline void RunLine(std::string l, const ue3::FVector& a_batman, bool a_quiet = false)
	{
		if (!g_px.dll) g_px.Load();
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
		if (l.empty() || l[0] == '#') return;
		if (!a_quiet) Log("> %s", l.c_str());
		std::vector<char> w(l.begin(), l.end());
		w.push_back(0);
		char* ctx = nullptr;
		char* c = strtok_s(w.data(), " \t", &ctx);
		char* a1 = strtok_s(nullptr, " \t", &ctx);
		char* a2 = strtok_s(nullptr, " \t", &ctx);
		char* a3 = strtok_s(nullptr, " \t", &ctx);
		if (!c) return;
		{
			if (!_stricmp(c, "mem") && a1) {
				HexDump(ParseAddr(a1), a2 ? strtoull(a2, nullptr, 16) : 0x100);
			} else if (!_stricmp(c, "ptr") && a1) {
				Log("  [%s] = %p", a1, reinterpret_cast<void*>(ReadOr<uintptr_t>(ParseAddr(a1), 0)));
			} else if (!_stricmp(c, "px") && a1 && !g_px.getPhysics) {
				Log("  PhysX not loaded");
			} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "info")) {
				PxInfo(a_batman);
			} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "mesh")) {
				PxMesh(a2 ? atoi(a2) : 3);
			} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "convex")) {
				PxConvex(a2 ? atoi(a2) : 3);
			} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "export") && a3) {
				PxExport(a_batman, static_cast<float>(atof(a2)), a3);
			} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "bexport") && a3) {
				BExportStart(a_batman, static_cast<float>(atof(a2)), a3);  // runs a few ms per tick (StepExport)
			} else if (!_stricmp(c, "exportbudget") && a1) {
				g_exportBudgetMs = std::max(0.5, atof(a1));
				Log("  PhysX export time %.1f ms per tick", g_exportBudgetMs);
			} else if (!_stricmp(c, "px") && a1 && !_stricmp(a1, "bforget")) {
				Log("  %zu sent shapes forgotten", g_sentShapes.size());
				g_sentShapes.clear();
				g_doneActors.clear();
				g_exportCount = 0;
			} else if (!_stricmp(c, "cam") && a1 && !_stricmp(a1, "fov") && a2) {
				g_mimicFov = !_stricmp(a2, "on");
				Log("  Arkham's field of view %s", g_mimicFov ? "follows Spider-Man's lens" : "is Arkham's own");
			} else if (!_stricmp(c, "cam") && a1 && !_stricmp(a1, "mimic") && a2) {
				g_mimicCamera = !_stricmp(a2, "on");
				Log("  Arkham's view %s", g_mimicCamera ? "follows Spider-Man's camera" : "is Arkham's own camera");
			} else if (!_stricmp(c, "shot")) {
				overlay::g_shotEvery = a2 ? std::max(1, atoi(a2)) : 1;
				overlay::g_shotsLeft = a1 ? std::max(1, std::min(600, atoi(a1))) : 1;
				Log("  %d shot(s), every %d frame(s) -> logs\\shots", overlay::g_shotsLeft.load(), overlay::g_shotEvery);
			} else if (!_stricmp(c, "overlay")) {
				overlay::Command(a1, a2, a3, strtok_s(nullptr, " \t", &ctx));
			} else if (!_stricmp(c, "grapple")) {
				if (a1 && (!_stricmp(a1, "on") || !_stricmp(a1, "off"))) grapple::g_enabled = !_stricmp(a1, "on");
				if (a1 && !_stricmp(a1, "pertick") && a2) grapple::g_perTick = std::max(500, atoi(a2));
				grapple::Status();
			} else if (!_stricmp(c, "batman") && a1) {
				g_batmanHide = !_stricmp(a1, "hide") ? 1 : !_stricmp(a1, "show") ? 0 : -1;
				Log("  Batman's model: %s", g_batmanHide == 1 ? "hidden" : g_batmanHide == 0 ? "shown" : "hidden while Spider-Man is drawn");
			} else if (!_stricmp(c, "tp") && a1 && a2 && a3) {
				g_tpFeet = { static_cast<float>(atof(a1)), static_cast<float>(atof(a2)), static_cast<float>(atof(a3)) };
				g_tpPending = true;
			} else if (!_stricmp(c, "scanextent") && a1) {
				g_scanExtent = static_cast<float>(atof(a1));
				Log("  tile scans sweep a %.0f UU box", g_scanExtent);
			} else if (!_stricmp(c, "scanbudget") && a1) {
				g_scanBudgetMs = atof(a1);
				Log("  scan budget %.1f ms per tick", g_scanBudgetMs);
			} else if (!_stricmp(c, "tscan") && a1 && a2 && a3) {
				char* v[6] = {};
				for (auto& t : v) t = strtok_s(nullptr, " \t", &ctx);
				if (!v[4]) {
					Log("  usage: tscan <name> <x0> <y0> <x1> <y1> <step> <top> <bottom> [filter] (UU)");
				} else {
					ScanJob s;
					s.tile = true;
					s.name = a1;
					s.x0 = static_cast<float>(atof(a2)), s.y0 = static_cast<float>(atof(a3));
					float x1 = static_cast<float>(atof(v[0])), y1 = static_cast<float>(atof(v[1]));
					s.stepUU = static_cast<float>(atof(v[2]));
					s.topUU = static_cast<float>(atof(v[3])), s.bottomUU = static_cast<float>(atof(v[4]));
					if (v[5]) s.filter = static_cast<uint8_t>(atoi(v[5]));
					s.nx = static_cast<int>((x1 - s.x0) / s.stepUU + 0.5f) + 1;
					s.ny = static_cast<int>((y1 - s.y0) / s.stepUU + 0.5f) + 1;
					std::wstring dir = StreamDir();
					s.final = dir + L"\\" + std::wstring(s.name.begin(), s.name.end()) + L".scan";
					s.part = s.final + L".part";
					if (s.stepUU > 0 && s.nx > 0 && s.ny > 0 && s.nx * s.ny <= 250000) g_scanQueue.push_back(std::move(s));
					else Log("  bad tile");
				}
			} else if (!_stricmp(c, "trace") && a1 && a2 && a3 && g_traceReqCount < 16) {
				char*  v[4] = {};
				for (auto& t : v) t = strtok_s(nullptr, " \t", &ctx);
				if (!v[2]) {
					Log("  usage: trace dx dy dz ex ey ez [filter] (meters from Batman)");
				} else {
					auto& r = g_traceReqs[g_traceReqCount++];
					r.start = { a_batman.x + static_cast<float>(atof(a1) * 100), a_batman.y + static_cast<float>(atof(a2) * 100),
						a_batman.z + static_cast<float>(atof(a3) * 100) };
					r.end = { a_batman.x + static_cast<float>(atof(v[0]) * 100), a_batman.y + static_cast<float>(atof(v[1]) * 100),
						a_batman.z + static_cast<float>(atof(v[2]) * 100) };
					r.filter = static_cast<uint8_t>(v[3] ? atoi(v[3]) : 66);
				}
			} else if (!_stricmp(c, "scan") && a1 && a2 && a3 && !g_scan.active) {
				char* a4 = strtok_s(nullptr, " \t", &ctx);
				std::wstring path = g_logDir + L"\\" + std::wstring(a3, a3 + strlen(a3)) + L".scan";
				FILE*        o = _wfopen(path.c_str(), L"w");
				if (!o) {
					Log("  can't write %ls", path.c_str());
				} else {
					g_scan = {};
					g_scan.center = a_batman;
					g_scan.radiusUU = static_cast<float>(atof(a1) * 100.0);
					g_scan.stepUU = static_cast<float>(atof(a2) * 100.0);
					g_scan.n = static_cast<int>(2 * g_scan.radiusUU / g_scan.stepUU) + 1;
					g_scan.out = o;
					g_scan.startMs = GetTickCount64();
					if (a4) g_scan.filter = static_cast<uint8_t>(atoi(a4));
					char* a5 = strtok_s(nullptr, " \t", &ctx);
					char* a6 = strtok_s(nullptr, " \t", &ctx);
					if (a5) g_scan.topUU = static_cast<float>(atof(a5) * 100.0);
					if (a6) g_scan.bottomUU = static_cast<float>(atof(a6) * 100.0);
					fprintf(o, "# ArkWeb collision scan, UU, Z up. Batman %.1f %.1f %.1f, radius %.0f, step %.0f, grid %d, filter %d\n", a_batman.x,
						a_batman.y, a_batman.z, g_scan.radiusUU, g_scan.stepUU, g_scan.n, g_scan.filter);
					g_scan.active = true;
					Log("  scan armed: %d x %d columns, step %.0f UU, radius %.0f UU, filter %d -> %ls", g_scan.n, g_scan.n, g_scan.stepUU, g_scan.radiusUU,
						g_scan.filter, path.c_str());
				}
			} else {
				Log("  unknown command");
			}
		}
	}

	// Runs pending ak_cmd.txt commands; called from the game thread with Batman's location.
	inline void Poll(const ue3::FVector& a_batman)
	{
		std::wstring file = g_logDir + L"\\ak_cmd.txt";
		if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return;
		FILE* f = _wfopen(file.c_str(), L"r");
		if (!f) return;
		std::vector<std::string> lines;
		char buf[512];
		while (fgets(buf, sizeof(buf), f)) lines.emplace_back(buf);
		fclose(f);
		DeleteFileW(file.c_str());
		for (auto& l : lines) RunLine(l, a_batman);
		Log("< done");
	}
}
