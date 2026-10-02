// Combat: when Arkham says Batman is fighting (the host's kHostCombat, from RPlayerControllerCombat.IsInCombat),
// Arkham plays him - its own fight, the controller's buttons and left stick - and Spider-Man stands in his place:
// pinned to Batman's feet every physics step, facing his way and taking his pose (mirror.h). Spider-Man gets only
// the right stick then (his camera is the view; the host turns Arkham's control rotation to it).
//
// The pin is the perch pin's (zip.h) with a moving target: once per fight the worker moves him to Batman (the
// teleport) and keeps the copies of his position; every physics step writes Batman's feet into them (only into
// copies still within 3 m of what was written last: anything farther is no longer his).
#pragma once

#include "gotham.h"
#include "mirror.h"
#include "../common/link.h"
#include "../common/util.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace arkweb::combat
{
	inline std::atomic<bool>     g_active{ false };
	inline std::atomic<int>      g_pinState{ 0 };  // 0 none, 1 the worker moves him there and finds the copies, 2 pinning
	inline uintptr_t             g_addr[12] = {};
	inline int                   g_count = 0;
	inline float                 g_last[3] = {};    // the position written last (pump thread)
	inline SRWLOCK               g_lock = SRWLOCK_INIT;
	inline double                g_target[3] = {};  // Batman's feet in New York's world
	inline std::atomic<uint32_t> g_fights{ 0 };

	// ---- the view: in a fight Arkham's own camera is the picture (the host's kHostView), so Spider-Man's camera
	// takes Arkham's and the captured Spider-Man lines up with it. Copying Spider-Man's camera instead (as out of
	// combat) showed a camera chasing a hero pinned to Batman every physics step: jerky and too close, and Arkham's
	// picture and the capture disagreed ("convulsing", 2026-10-02). The camera manager's update (exe+897d30; this,
	// dt, two flags) writes the final camera once a frame, the only writer (`mirror watchcam`): manager +0x744,
	// rows side/up/forward then the position, 4 floats each. Right after it, Arkham's goes in.
	constexpr uintptr_t kCamUpdate = 0x897d30;
	constexpr uint8_t   kCamUpdateSig[16] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x44, 0x88, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x55 };
	constexpr int       kCamMatrix = 0x744;
	using CamUpdateFn = uint64_t (*)(void*, float, uint64_t, uint64_t);
	inline CamUpdateFn           g_origCamUpdate = nullptr;
	inline void* volatile*       g_camManager = nullptr;  // main.cpp's g_heroCamera
	inline std::atomic<bool>     g_viewOn{ false };
	inline SRWLOCK               g_viewLock = SRWLOCK_INIT;
	inline float                 g_viewRows[3][3] = {};
	inline double                g_viewPos[3] = {};
	inline std::atomic<uint64_t> g_viewWrites{ 0 };

	inline uint64_t CamUpdateDetour(void* a_this, float a_dt, uint64_t a_r8, uint64_t a_r9)
	{
		uint64_t r = g_origCamUpdate(a_this, a_dt, a_r8, a_r9);
		if (g_viewOn.load(std::memory_order_relaxed) && g_camManager) {
			auto cam = reinterpret_cast<uintptr_t>(*g_camManager);
			if (cam) {
				float rows[3][3], pos[3];
				AcquireSRWLockShared(&g_viewLock);
				std::memcpy(rows, g_viewRows, sizeof(rows));
				for (int k = 0; k < 3; ++k) pos[k] = static_cast<float>(g_viewPos[k]);
				ReleaseSRWLockShared(&g_viewLock);
				for (int i = 0; i < 3; ++i) SafeWrite(reinterpret_cast<void*>(cam + kCamMatrix + i * 0x10), rows[i], sizeof(rows[i]));
				SafeWrite(reinterpret_cast<void*>(cam + kCamMatrix + 0x30), pos, sizeof(pos));
				++g_viewWrites;
			}
		}
		return r;
	}

	inline void Install(void* volatile* a_camManager)
	{
		g_camManager = a_camManager;
		g_origCamUpdate = reinterpret_cast<CamUpdateFn>(InlineHook(g_exe + kCamUpdate, kCamUpdateSig, sizeof(kCamUpdateSig), reinterpret_cast<void*>(&CamUpdateDetour)));
		Log("combat: camera update hook %s (in fights Spider-Man's camera is Arkham's)", g_origCamUpdate ? "ready" : "FAILED - this build doesn't match");
	}

	inline void Target(double a_out[3])
	{
		AcquireSRWLockShared(&g_lock);
		for (int k = 0; k < 3; ++k) a_out[k] = g_target[k];
		ReleaseSRWLockShared(&g_lock);
	}

	// Pump (every physics step, game thread): the host's state.
	inline void Step(const proto::HostState& a_hs)
	{
		bool on = (a_hs.flags & proto::kHostCombat) && gotham::g_anchorValid;
		double t[3];
		for (int k = 0; k < 3; ++k) t[k] = a_hs.puppetPos[k] - gotham::g_anchorGuest[k] + gotham::g_place[k];  // anchored -> New York
		AcquireSRWLockExclusive(&g_lock);
		for (int k = 0; k < 3; ++k) g_target[k] = t[k];
		ReleaseSRWLockExclusive(&g_lock);
		mirror::g_faceYaw.store(a_hs.puppetYaw, std::memory_order_relaxed);
		// the view: Arkham's camera, anchored -> New York's world like Batman
		bool view = on && (a_hs.flags & proto::kHostView);
		if (view) {
			AcquireSRWLockExclusive(&g_viewLock);
			for (int i = 0; i < 3; ++i)
				for (int k = 0; k < 3; ++k) g_viewRows[i][k] = a_hs.viewRot[i * 3 + k];
			for (int k = 0; k < 3; ++k) g_viewPos[k] = a_hs.viewPos[k] - gotham::g_anchorGuest[k] + gotham::g_place[k];
			ReleaseSRWLockExclusive(&g_viewLock);
		}
		if (view != g_viewOn.load(std::memory_order_relaxed)) {
			g_viewOn = view;
			Log(view ? "combat: Spider-Man's camera follows Arkham's" : "combat: Spider-Man's camera is his own again");
		}
		if (on != g_active.load(std::memory_order_relaxed)) {
			g_active = on;
			mirror::g_combat = on;
			g_pinState = on ? 1 : 0;
			if (on) ++g_fights;
			Log(on ? "combat: Batman is fighting - Spider-Man takes his place and his moves" : "combat over: Spider-Man is his own again");
		}
		if (g_pinState.load(std::memory_order_acquire) != 2) return;
		const float f[3] = { static_cast<float>(t[0]), static_cast<float>(t[1]), static_cast<float>(t[2]) };
		for (int i = 0; i < g_count; ++i) {
			float v[3];
			if (!SafeRead(v, reinterpret_cast<const void*>(g_addr[i]), sizeof(v))) continue;
			float d = std::fabs(v[0] - g_last[0]) + std::fabs(v[1] - g_last[1]) + std::fabs(v[2] - g_last[2]);
			if (d < 3.0f) SafeWrite(reinterpret_cast<void*>(g_addr[i]), f, sizeof(f));
		}
		std::memcpy(g_last, f, sizeof(f));
	}

	// Worker: after moving him to Batman, the copies of his position (a_pos: where he is now).
	inline void SetPin(const std::vector<uintptr_t>& a_copies, const float a_pos[3])
	{
		if (g_pinState.load() != 1) return;
		if (a_copies.empty() || a_copies.size() > 12) {
			Log("combat: %zu copies of his position found - no pin this time", a_copies.size());
			g_pinState = 0;
			return;
		}
		g_count = static_cast<int>(a_copies.size());
		for (int i = 0; i < g_count; ++i) g_addr[i] = a_copies[i];
		std::memcpy(g_last, a_pos, sizeof(g_last));
		g_pinState.store(2, std::memory_order_release);
		Log("combat: Spider-Man pinned to Batman (%d copies of his position)", g_count);
	}
}
