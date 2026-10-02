// Zip to point: Arkham's grapple ledges as Spider-Man point-launch targets.
//
// Arkham's grapple points (RGrapplePoint actors and the arrays of RGrapplePointCollection) are ledge
// segments - two end points on the edge and the outward normal of the wall under it - exactly where
// Batman's grapple pulls him up to. The host writes every one loaded around Batman to
// logs\stream\grapple.awg (ak_host/grapple.h). This side reads the file whenever it changes, picks the
// point on a ledge the camera looks at most directly, and on L2 + R2 starts Spider-Man's own point
// launch to it.
//
// The launch is the game's: HeroStateZipToPointLaunch, entered through the hero's state machine the
// way HeroTransitionManager::TransitionPointLaunch (vtable slot 88, exe+97db80) does it for a perch:
// the transition data from its constructor (exe+b139d0) and setters (position exe+b15b70, direction
// exe+b15af0, animations exe+b158d0, launch exe+b15750 - the last two given the hero matrix at
// TransitionManager +0x68), then SyncStateMachine::RequestTransition ([TransitionManager +0x50],
// vtable slot 10, exe+20df4a0) with the state's descriptor exe+6dfbc80. The request is made inside the
// game's own call of slot 88 (patched): the hero states call it when they see the point-launch input,
// so it runs where the game's own point launches start. If no such call comes within a moment, the
// pump makes it (`zip fallback on|off`).
#pragma once

#include "gotham.h"
#include "../common/coords.h"
#include "../common/util.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace arkweb::zip
{
	// ---- Spider-Man.exe 4.0630 ------------------------------------------------------------------------
	constexpr uintptr_t kTmVtable = 0x38b4350;            // Hero::HeroTransitionManager
	constexpr int       kTmSlotPointLaunch = 88;          // bool TransitionPointLaunch(this, u32 flags, u32)
	constexpr uintptr_t kTmPointLaunch = 0x97db80;
	constexpr int       kTmEntity = 0x8, kTmStateMachine = 0x50, kTmHeroMatrix = 0x68;  // hero matrix: rows side/up/fwd, position +0x30
	constexpr uintptr_t kSyncSmVtable = 0x4f84148, kSmVtable = 0x4f83f48;
	constexpr int       kSmSlotRequest = 10;              // bool RequestTransition(this, const StateDescriptor*, TransitionData*)
	constexpr uintptr_t kSyncSmRequest = 0x20df4a0, kSmRequest = 0x20dc9c0;
	constexpr uintptr_t kZipDescriptor = 0x6dfbc80;       // HeroStateZipToPointLaunch's state descriptor
	constexpr uintptr_t kJumpDescriptor = 0x6deda80;      // HeroStateJump's: the zip's launch at its end (exe+b1a7e4)
	constexpr uintptr_t kJumpLocalDescriptor = 0x6deece0; // HeroStateJumpLocal (its driver)
	constexpr uintptr_t kZipLocalDescriptor = 0x6dfbd80, kZipRemoteDescriptor = 0x6dfbe80;  // the zip's drivers
	// The zip that completes asks for HeroStatePerchIdle. Its driver HeroStatePerchIdleLocal checks every frame
	// (exe+ad4890, last of its exits) for the perch's target entity (+0x120, from the transition data +0x24)
	// and, with none, calls HeroTransitionManager::TransitionFall (slot 20, exe+96e580) forced: HeroStateFall.
	constexpr uintptr_t kPerchIdleDescriptor = 0x6df4330, kPerchIdleUpdateDescriptor = 0x6df4420;
	constexpr uintptr_t kPerchEnterDescriptor = 0x6df40d0, kPerchEnterLocalDescriptor = 0x6df41d0;
	constexpr uintptr_t kFallDescriptor = 0x6ded020, kFallLocalDescriptor = 0x6ded130;
	constexpr uintptr_t kZipStateId = 0x6dfbcfc;          // its id (CRC of the name), set when the state registers
	constexpr uintptr_t kDataCtor = 0xb139d0, kSetPos = 0xb15b70, kSetDir = 0xb15af0, kSetAnim = 0xb158d0, kSetLaunch = 0xb15750;
	constexpr size_t    kDataBytes = 0x100;               // the game's is 0x84 (byte +4); flags at +0x70..+0x83

	struct Sig
	{
		uintptr_t rva;
		uint8_t   bytes[12];
	};
	constexpr Sig kSigs[] = {
		{ kDataCtor, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48 } },
		{ kSetPos, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x4c, 0x8d, 0x41 } },
		{ kSetDir, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x4c, 0x8d, 0x41 } },
		{ kSetAnim, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x80, 0x79 } },
		{ kSetLaunch, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89 } },
		{ kTmPointLaunch, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x44, 0x89, 0x40, 0x18, 0x55 } },
		{ kSyncSmRequest, { 0x40, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0x70, 0x02, 0x00, 0x00, 0x49 } },
		{ kSmRequest, { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56 } },
	};

	using CtorFn = void (*)(void*);
	using SetVecFn = void (*)(void*, const float*);
	using SetAnimFn = void (*)(void*, const float*, bool, bool);
	using RequestFn = bool (*)(void*, const void*, void*);
	using PointLaunchFn = bool (*)(void*, uint32_t, uint32_t);

	// ---- grapple ledges (absolute guest space: Arkham / 100, like the streamed tiles) ---------------------
	// "AWG1", u32 count; per ledge f32 a[3], b[3] (UU, Arkham), f32 outward normal[3], u32 flags (host's).
	struct Edge
	{
		double   a[3], b[3];
		float    n[3];
		uint32_t flags;
	};
	inline std::vector<Edge> g_edges;  // worker thread only
	inline FILETIME          g_fileTime{};
	inline std::atomic<int>  g_edgeCount{ 0 };
	inline ULONGLONG         g_lastFileCheck = 0;

	// ---- tuning (`zip ...`) ------------------------------------------------------------------------------
	inline std::atomic<bool>  g_enabled{ true };
	inline std::atomic<float> g_maxAngleDeg{ 15.0f };  // from the view line (SM's MaxViewAngleForZipTarget is of this order)
	inline std::atomic<float> g_minDist{ 4.0f }, g_maxDist{ 60.0f };
	inline std::atomic<float> g_inset{ 0.15f };        // meters onto the roof from the ledge line
	inline std::atomic<float> g_lift{ 0.05f };         // meters above it
	inline std::atomic<int>   g_dirMode{ 1 };          // transition direction: 0 none, 1 travel, 2 ledge outward, 3 ledge inward
	inline std::atomic<bool>  g_fallback{ true };      // the pump makes the request when slot 88 isn't called
	inline std::atomic<int>   g_fallbackMs{ 120 };
	inline int16_t            g_flagOverride[0x14];    // bytes +0x70..+0x83 of the transition data, -1 = the setters' value
	inline std::atomic<bool>  g_flagsSet{ false };

	// ---- state ---------------------------------------------------------------------------------------------
	struct Target
	{
		bool   valid = false;
		double pos[3] = {};  // New York's world (where the hero is)
		double abs[3] = {};  // absolute guest space (anchored, for the host's marker)
		float  dir[3] = {};  // the transition direction for it
		int    edge = -1;
		float  score = 0.0f;
		double dist = 0.0;
	};
	inline SRWLOCK            g_lock = SRWLOCK_INIT;
	inline Target             g_target;           // the current one (worker writes)
	inline Target             g_request;          // the one L2 + R2 asked for
	inline std::atomic<bool>  g_pending{ false };
	inline std::atomic<ULONGLONG> g_requestAt{ 0 };
	inline std::atomic<uintptr_t> g_tm{ 0 };      // the hero's HeroTransitionManager
	inline std::atomic<bool>  g_ready{ false };   // signatures matched and slot 88 patched
	inline PointLaunchFn      g_origPointLaunch = nullptr;
	inline std::atomic<uint32_t> g_started{ 0 }, g_failed{ 0 }, g_viaSlot{ 0 }, g_viaPump{ 0 };
	inline std::atomic<ULONGLONG> g_activeUntil{ 0 };
	// Perch: with no ledge path or perch entity behind the target (Gotham's grapple points have neither) the
	// zip can't land on it the way Spider-Man's own perch points do. It ends in its launch (HeroStateJump) or
	// in HeroStatePerchIdle, which drops him at once (TransitionFall, see kPerchIdleDescriptor). So after our
	// zip the launch is refused until A is pressed, and once he is perched the fall is refused until A (the
	// perch's own jump goes ahead) or B: he stays on the point. Every hero transition goes through
	// SyncStateMachine::RequestTransition (vtable slot 10, patched).
	using SmRequestFn = bool (*)(void*, const void*, void*);
	inline SmRequestFn             g_origSmRequest = nullptr;   // SyncStateMachine (the transition manager's machine)
	inline SmRequestFn             g_origBaseRequest = nullptr; // StateMachine (state drivers may use a plain one)
	inline std::atomic<int>        g_reqLogged{ 0 };            // hero requests logged since our last zip (diagnosis)
	inline std::atomic<uintptr_t>  g_lastLogged{ 0 };           // ... a request repeated every frame is logged once
	inline std::atomic<bool>       g_perch{ true };          // `zip perch on|off`
	inline std::atomic<ULONGLONG>  g_holdSince{ 0 };         // a zip of ours started then (0: no hold)
	inline std::atomic<ULONGLONG>  g_perchedAt{ 0 };         // ... and he went into HeroStatePerchIdle then (0: not yet)
	inline std::atomic<ULONGLONG>  g_aPressedAt{ 0 };        // A's last press (the virtual pad)
	inline std::atomic<ULONGLONG>  g_bPressedAt{ 0 };        // B's
	inline std::atomic<bool>       g_holdLogged{ false };
	// The pin: HeroStatePerchIdle switches gravity off but keeps the speed the zip arrived with, so he slid on
	// (7 m/s down after a downward zip, 2026-10-02). Once he is perched the worker finds his position's copies
	// the way the teleport does (main.cpp, FindPositionCopies) and every physics step puts them back.
	inline std::atomic<int>        g_pinState{ 0 };       // 0 none, 1 wanted (the worker looks), 2 pinning
	inline std::atomic<ULONGLONG>  g_pinFor{ 0 };         // the perch (its g_perchedAt) the pin belongs to
	inline uintptr_t               g_pinAddr[12] = {};
	inline int                     g_pinCount = 0, g_pinTries = 0;
	inline float                   g_pinPos[3] = {};
	inline std::atomic<uint32_t>   g_perches{ 0 }, g_launches{ 0 };
	inline std::atomic<bool>  g_tmScanning{ false };
	inline ULONGLONG          g_lastTmScan = 0;
	inline void* volatile*    g_heroLocal = nullptr;  // main.cpp's captured Hero::HeroLocal

	inline uintptr_t HeroEntity() { return g_heroLocal ? ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(*g_heroLocal) + 0x8, 0) : 0; }

	inline bool ValidTm(uintptr_t a_tm)
	{
		if (!a_tm || ReadOr<uintptr_t>(a_tm, 0) != g_exe + kTmVtable) return false;
		uintptr_t ent = HeroEntity();
		if (!ent || ReadOr<uintptr_t>(a_tm + kTmEntity, 0) != ent) return false;
		uintptr_t sm = ReadOr<uintptr_t>(a_tm + kTmStateMachine, 0);
		uintptr_t vt = ReadOr<uintptr_t>(sm, 0);
		return vt == g_exe + kSyncSmVtable || vt == g_exe + kSmVtable;
	}

	inline bool SmRequestDetour(void* a_sm, const void* a_desc, void* a_data);
	inline bool BaseRequestDetour(void* a_sm, const void* a_desc, void* a_data);

	// ---- the request ----------------------------------------------------------------------------------------
	// Game thread (slot 88's caller, or the pump). Returns the state machine's answer.
	inline bool Execute(uintptr_t a_tm, const Target& a_t, const char* a_via)
	{
		if (!g_ready.load() || !ValidTm(a_tm) || ReadOr<uint32_t>(g_exe + kZipStateId, 0) == 0) {
			++g_failed;
			Log("zip: no request (%s): %s", a_via, !g_ready ? "not ready" : !ValidTm(a_tm) ? "no valid transition manager" : "state not registered");
			return false;
		}
		uintptr_t sm = ReadOr<uintptr_t>(a_tm + kTmStateMachine, 0);
		auto      request = reinterpret_cast<RequestFn>(ReadOr<uintptr_t>(ReadOr<uintptr_t>(sm, 0) + kSmSlotRequest * 8, 0));
		if (request == reinterpret_cast<RequestFn>(&SmRequestDetour)) request = g_origSmRequest;  // patched classes: their original
		if (request == reinterpret_cast<RequestFn>(&BaseRequestDetour)) request = g_origBaseRequest;
		if (!request || !InModule(reinterpret_cast<uintptr_t>(request), g_exe)) return false;
		alignas(16) uint8_t d[kDataBytes] = {};
		float               pos[4] = { static_cast<float>(a_t.pos[0]), static_cast<float>(a_t.pos[1]), static_cast<float>(a_t.pos[2]), 0.0f };
		float               dir[4] = { a_t.dir[0], a_t.dir[1], a_t.dir[2], 0.0f };
		const auto*         heroMat = reinterpret_cast<const float*>(a_tm + kTmHeroMatrix);
		bool                ok = false;
		__try {
			hk::Fn<CtorFn>(kDataCtor)(d);
			hk::Fn<SetVecFn>(kSetPos)(d, pos);  // no target actor: world position as it is
			if (g_dirMode.load() != 0) hk::Fn<SetVecFn>(kSetDir)(d, dir);
			if (g_flagsSet.load()) {
				for (int i = 0; i < 0x14; ++i) {
					if (g_flagOverride[i] >= 0) d[0x70 + i] = static_cast<uint8_t>(g_flagOverride[i]);
				}
			}
			hk::Fn<SetAnimFn>(kSetAnim)(d, heroMat, false, true);  // as TransitionPointLaunch does for a perch with no flags
			hk::Fn<SetVecFn>(kSetLaunch)(d, heroMat);
			ok = request(reinterpret_cast<void*>(sm), reinterpret_cast<const void*>(g_exe + kZipDescriptor), d);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Log("zip: the request raised an exception (%s)", a_via);
			ok = false;
		}
		const float* hp = heroMat + 12;
		if (ok) {
			++g_started;
			g_activeUntil = GetTickCount64() + 2500;
			g_perchedAt = 0;
			g_holdSince = GetTickCount64();
			g_holdLogged = false;
			g_reqLogged = 0;
			g_lastLogged = 0;
		} else {
			++g_failed;
		}
		Log("zip: point launch to (%.2f %.2f %.2f), %.1f m from the hero at (%.2f %.2f %.2f), via %s: %s", pos[0], pos[1], pos[2], a_t.dist, hp[0], hp[1],
			hp[2], a_via, ok ? "started" : "REFUSED by the state machine");
		return ok;
	}

	inline bool TakePending(Target& a_out)
	{
		if (!g_pending.load(std::memory_order_acquire)) return false;
		AcquireSRWLockExclusive(&g_lock);
		bool have = g_pending.exchange(false);
		if (have) a_out = g_request;
		ReleaseSRWLockExclusive(&g_lock);
		return have && GetTickCount64() - g_requestAt.load() < 400;  // a stale press does nothing
	}

	// Is this machine the hero's? Every state machine is a component: +0x8 is the entity it belongs to.
	// (The zip's own states ask their machine - not necessarily the one at TransitionManager +0x50.)
	inline bool HeroMachine(void* a_sm)
	{
		uintptr_t ent = HeroEntity();
		return ent && ReadOr<uintptr_t>(reinterpret_cast<uintptr_t>(a_sm) + 0x8, 0) == ent;
	}

	enum class Req { kOther, kZip, kPerch, kJump, kFall };

	inline Req Classify(const void* a_desc)
	{
		uintptr_t d = reinterpret_cast<uintptr_t>(a_desc) - g_exe;
		if (d == kZipDescriptor || d == kZipLocalDescriptor || d == kZipRemoteDescriptor) return Req::kZip;
		if (d == kPerchIdleDescriptor || d == kPerchIdleUpdateDescriptor || d == kPerchEnterDescriptor || d == kPerchEnterLocalDescriptor) return Req::kPerch;
		if (d == kJumpDescriptor || d == kJumpLocalDescriptor) return Req::kJump;
		if (d == kFallDescriptor || d == kFallLocalDescriptor) return Req::kFall;
		return Req::kOther;
	}

	// true: refuse the request (the hero stays on the point)
	inline bool HoldRequest(void* a_sm, const void* a_desc, bool a_base)
	{
		ULONGLONG since = g_holdSince.load(std::memory_order_relaxed);
		if (!since || !HeroMachine(a_sm)) return false;
		ULONGLONG now = GetTickCount64(), perched = g_perchedAt.load(std::memory_order_relaxed);
		Req       r = Classify(a_desc);
		bool      on = g_perch.load(std::memory_order_relaxed);
		// the zip's launch: until A (pressed during the zip or after); the perch's fall: until A or B after it
		bool hold = on && ((r == Req::kJump && g_aPressedAt.load() < since + 150 && now - since < 60000) ||
			(r == Req::kFall && perched && g_aPressedAt.load() < perched && g_bPressedAt.load() < perched && now - perched < 600000));
		auto desc = reinterpret_cast<uintptr_t>(a_desc);
		if ((now - since < 6000 || perched) && g_lastLogged.exchange(desc) != desc && g_reqLogged.fetch_add(1) < 32) {
			Log("zip: hero asks for state exe+%llx (%s machine %p) %.2f s after the zip%s", static_cast<unsigned long long>(desc - g_exe),
				a_base ? "plain" : "sync", a_sm, (now - since) / 1000.0, hold ? " - REFUSED (perched)" : "");
		}
		if (hold && !g_holdLogged.exchange(true)) {
			++g_perches;
			Log(r == Req::kFall ? "zip: perched on the point - A jumps, B drops" : "zip: perched on the point (launch held) - A launches");
		}
		if (hold && r == Req::kFall && g_pinFor.load() != perched) {
			g_pinFor = perched;  // this perch's pin: the worker finds the copies of his position
			g_pinTries = 0;
			g_pinState = 1;
		}
		return hold;
	}

	// Worker thread: the copies of his position it found for the pin (a_pos: where he is now).
	inline void SetPin(const std::vector<uintptr_t>& a_copies, const float a_pos[3])
	{
		if (g_pinState.load() != 1) return;
		if (!g_perchedAt.load() || g_pinFor.load() != g_perchedAt.load()) {
			g_pinState = 0;  // the perch ended meanwhile
			return;
		}
		// a few, not many: many matches are a common value, and writing them all would corrupt the game
		if (a_copies.empty() || a_copies.size() > 12) {
			if (++g_pinTries >= 8) {
				g_pinState = 0;
				Log("zip: no pin for the perch (%zu copies of his position found)", a_copies.size());
			}
			return;  // he moved between reading and scanning: again next round
		}
		g_pinCount = static_cast<int>(a_copies.size());
		for (int i = 0; i < g_pinCount; ++i) g_pinAddr[i] = a_copies[i];
		for (int i = 0; i < 3; ++i) g_pinPos[i] = a_pos[i];
		g_pinState.store(2, std::memory_order_release);
		Log("zip: pinned on the perch at (%.2f %.2f %.2f), %d copies of his position", a_pos[0], a_pos[1], a_pos[2], g_pinCount);
	}

	// Every physics step (the pump): the perch's position back into its copies, until the perch ends.
	inline void PinStep()
	{
		if (g_pinState.load(std::memory_order_acquire) != 2) return;
		ULONGLONG perched = g_perchedAt.load(std::memory_order_relaxed);
		if (!perched || g_pinFor.load(std::memory_order_relaxed) != perched) {
			g_pinState = 0;
			return;
		}
		for (int i = 0; i < g_pinCount; ++i) {
			float v[3];
			if (!SafeRead(v, reinterpret_cast<const void*>(g_pinAddr[i]), sizeof(v))) continue;
			float d = std::fabs(v[0] - g_pinPos[0]) + std::fabs(v[1] - g_pinPos[1]) + std::fabs(v[2] - g_pinPos[2]);
			if (d > 1e-4f && d < 3.0f) SafeWrite(reinterpret_cast<void*>(g_pinAddr[i]), g_pinPos, sizeof(g_pinPos));  // farther: not his any more
		}
	}

	// Before a request the hold lets through: a perch counts from now on - the perch's first check may ask
	// for the fall before the request returns. Returns what to restore if the machine refuses it.
	inline ULONGLONG BeforeRequest(void* a_sm, const void* a_desc)
	{
		ULONGLONG was = g_perchedAt.load();
		if (!was && Classify(a_desc) == Req::kPerch && HeroMachine(a_sm)) {
			g_holdLogged = false;  // the first refusal of the fall says so
			g_perchedAt = GetTickCount64();
		}
		return was;
	}

	// After a request the hold let through: what the machine accepted decides whether the hold goes on.
	inline void AfterRequest(void* a_sm, const void* a_desc, bool a_ok, ULONGLONG a_perchedBefore)
	{
		if (!a_ok && Classify(a_desc) == Req::kPerch) g_perchedAt = a_perchedBefore;  // not perched after all
		ULONGLONG since = g_holdSince.load(std::memory_order_relaxed);
		if (!since || !a_ok || !HeroMachine(a_sm)) return;
		ULONGLONG now = GetTickCount64();
		switch (Classify(a_desc)) {
		case Req::kZip:
		case Req::kPerch:
			break;
		case Req::kJump:
			++g_launches;
			g_holdSince = 0, g_perchedAt = 0;  // the launch (or the perch's jump) goes ahead
			break;
		default:
			// the zip ended some other way: a swing, a new state, a fall (a zip cut short) or the drop off the
			// perch - not the leftovers of the state the zip started from
			if (now - since > 400) g_holdSince = 0, g_perchedAt = 0;
			break;
		}
	}

	inline bool SmRequestDetour(void* a_sm, const void* a_desc, void* a_data)
	{
		if (!g_holdSince.load(std::memory_order_relaxed)) return g_origSmRequest(a_sm, a_desc, a_data);
		if (HoldRequest(a_sm, a_desc, false)) return false;
		ULONGLONG was = BeforeRequest(a_sm, a_desc);
		bool      ok = g_origSmRequest(a_sm, a_desc, a_data);
		AfterRequest(a_sm, a_desc, ok, was);
		return ok;
	}

	inline bool BaseRequestDetour(void* a_sm, const void* a_desc, void* a_data)
	{
		if (!g_holdSince.load(std::memory_order_relaxed)) return g_origBaseRequest(a_sm, a_desc, a_data);
		if (HoldRequest(a_sm, a_desc, true)) return false;
		ULONGLONG was = BeforeRequest(a_sm, a_desc);
		bool      ok = g_origBaseRequest(a_sm, a_desc, a_data);
		AfterRequest(a_sm, a_desc, ok, was);
		return ok;
	}

	// Slot 88: the hero states call it when the point-launch input happens (and the game then looks for
	// its own perch target, of which Gotham has none).
	inline bool PointLaunchDetour(void* a_tm, uint32_t a_flags, uint32_t a_x)
	{
		auto tm = reinterpret_cast<uintptr_t>(a_tm);
		if (g_tm.load(std::memory_order_relaxed) != tm && ValidTm(tm)) g_tm = tm;
		Target t;
		if (TakePending(t)) {
			++g_viaSlot;
			if (Execute(tm, t, "the game's point-launch call")) return true;
		}
		return g_origPointLaunch(a_tm, a_flags, a_x);
	}

	// From the pump (game thread) every physics step: a press slot 88 didn't take.
	inline void PumpFallback()
	{
		if (!g_pending.load(std::memory_order_relaxed) || !g_fallback.load(std::memory_order_relaxed)) return;
		if (GetTickCount64() - g_requestAt.load() < static_cast<ULONGLONG>(g_fallbackMs.load())) return;
		uintptr_t tm = g_tm.load();
		Target    t;
		if (!TakePending(t)) return;
		if (GetTickCount64() < g_activeUntil.load()) return;  // a launch is under way: don't restart it
		++g_viaPump;
		Execute(tm, t, "the pump");
	}

	// ---- finding the transition manager (a background scan of the heap, like the follow camera) ---------
	inline void FindTm()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
		const uintptr_t          want = g_exe + kTmVtable;
		MEMORY_BASIC_INFORMATION mbi;
		std::vector<uint8_t>     buf(1 << 20);
		int                      found = 0;
		uintptr_t                hit = 0;
		ULONGLONG                t0 = GetTickCount64();
		for (uint8_t* a = nullptr; VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi); a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
			if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) ||
				!(mbi.Protect & PAGE_READWRITE) || mbi.RegionSize > (256u << 20))
				continue;
			for (SIZE_T off = 0; off < mbi.RegionSize; off += buf.size()) {
				SIZE_T n = std::min<SIZE_T>(buf.size(), mbi.RegionSize - off);
				auto*  base = static_cast<uint8_t*>(mbi.BaseAddress) + off;
				if (!SafeRead(buf.data(), base, n)) break;
				const auto* q = reinterpret_cast<const uintptr_t*>(buf.data());
				for (SIZE_T i = 0; i < n / 8; ++i) {
					if (q[i] != want) continue;
					uintptr_t obj = reinterpret_cast<uintptr_t>(base) + i * 8;
					++found;
					if (ValidTm(obj)) hit = obj;
				}
			}
		}
		if (hit) g_tm = hit;
		uintptr_t smVt = hit ? ReadOr<uintptr_t>(ReadOr<uintptr_t>(hit + kTmStateMachine, 0), 0) : 0;
		Log("zip: hero transition manager %p (%d objects of its class, %.1f s), state machine %s", reinterpret_cast<void*>(hit), found,
			(GetTickCount64() - t0) / 1000.0, smVt == g_exe + kSyncSmVtable ? "SyncStateMachine (perch hold works)" : smVt ? "StateMachine (NO perch hold)" : "-");
	}

	inline DWORD WINAPI TmScanThread(void*)
	{
		FindTm();
		g_tmScanning = false;
		return 0;
	}

	// ---- install (once, from InstallHooks) -------------------------------------------------------------------
	inline void Install()
	{
		for (const Sig& s : kSigs) {
			uint8_t b[12];
			if (!SafeRead(b, reinterpret_cast<const void*>(g_exe + s.rva), sizeof(b)) || memcmp(b, s.bytes, sizeof(b)) != 0) {
				Log("zip: exe+%llx doesn't match this build - zip to point disabled", static_cast<unsigned long long>(s.rva));
				return;
			}
		}
		auto* slot = reinterpret_cast<uintptr_t*>(g_exe + kTmVtable + kTmSlotPointLaunch * 8);
		auto* smSlot = reinterpret_cast<const uintptr_t*>(g_exe + kSyncSmVtable + kSmSlotRequest * 8);
		if (*slot != g_exe + kTmPointLaunch || *smSlot != g_exe + kSyncSmRequest) {
			Log("zip: vtable slots don't hold the expected functions - zip to point disabled");
			return;
		}
		DWORD prot;
		if (!VirtualProtect(slot, 8, PAGE_READWRITE, &prot)) {
			Log("zip: can't patch the transition manager's vtable - zip to point disabled");
			return;
		}
		g_origPointLaunch = reinterpret_cast<PointLaunchFn>(*slot);
		*slot = reinterpret_cast<uintptr_t>(&PointLaunchDetour);
		VirtualProtect(slot, 8, prot, &prot);
		auto* rq = const_cast<uintptr_t*>(smSlot);
		if (VirtualProtect(rq, 8, PAGE_READWRITE, &prot)) {
			g_origSmRequest = reinterpret_cast<SmRequestFn>(*rq);
			*rq = reinterpret_cast<uintptr_t>(&SmRequestDetour);
			VirtualProtect(rq, 8, prot, &prot);
		}
		auto* brq = reinterpret_cast<uintptr_t*>(g_exe + kSmVtable + kSmSlotRequest * 8);
		if (*brq == g_exe + kSmRequest && VirtualProtect(brq, 8, PAGE_READWRITE, &prot)) {
			g_origBaseRequest = reinterpret_cast<SmRequestFn>(*brq);
			*brq = reinterpret_cast<uintptr_t>(&BaseRequestDetour);
			VirtualProtect(brq, 8, prot, &prot);
		}
		for (auto& f : g_flagOverride) f = -1;
		g_ready = true;
		Log("zip: point launch hook ready (HeroTransitionManager slot 88 patched; state machine requests: sync %s, plain %s)", g_origSmRequest ? "hooked" : "NOT hooked",
			g_origBaseRequest ? "hooked" : "NOT hooked");
	}

	// ---- the ledges -----------------------------------------------------------------------------------------
	inline std::wstring EdgeFile() { return g_logDir + L"\\stream\\grapple.awg"; }

	// Worker thread, about once a second.
	inline void LoadIfChanged()
	{
		ULONGLONG now = GetTickCount64();
		if (now - g_lastFileCheck < 1000) return;
		g_lastFileCheck = now;
		WIN32_FILE_ATTRIBUTE_DATA fa;
		if (!GetFileAttributesExW(EdgeFile().c_str(), GetFileExInfoStandard, &fa)) return;
		if (CompareFileTime(&fa.ftLastWriteTime, &g_fileTime) == 0) return;
		FILE* f = _wfopen(EdgeFile().c_str(), L"rb");
		if (!f) return;
		char     magic[4];
		uint32_t n = 0;
		std::vector<Edge> edges;
		if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "AWG1", 4) && fread(&n, 4, 1, f) == 1 && n <= 200000) {
			edges.reserve(n);
			struct Raw
			{
				float    a[3], b[3], n[3];
				uint32_t flags;
			} r;
			for (uint32_t i = 0; i < n && fread(&r, sizeof(r), 1, f) == 1; ++i) {
				Edge       e;
				coords::V3 ga = coords::HostPosToGuest({ r.a[0], r.a[1], r.a[2] }), gb = coords::HostPosToGuest({ r.b[0], r.b[1], r.b[2] });
				coords::V3 gn = coords::HostDirToGuest({ r.n[0], r.n[1], r.n[2] });
				e.a[0] = ga.x, e.a[1] = ga.y, e.a[2] = ga.z;
				e.b[0] = gb.x, e.b[1] = gb.y, e.b[2] = gb.z;
				e.n[0] = static_cast<float>(gn.x), e.n[1] = static_cast<float>(gn.y), e.n[2] = static_cast<float>(gn.z);
				e.flags = r.flags;
				edges.push_back(e);
			}
		}
		fclose(f);
		g_fileTime = fa.ftLastWriteTime;
		if (edges.empty() && n) return;  // half-written: try again next time
		static int s_loads = 0;
		if (s_loads++ < 3 || edges.size() != g_edges.size()) Log("zip: %zu grapple ledges from Arkham", edges.size());
		g_edges = std::move(edges);
		g_edgeCount = static_cast<int>(g_edges.size());
	}

	// ---- targeting (worker thread) ---------------------------------------------------------------------------
	inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	// a_cam: rows side, up, forward, position (New York's world); a_hero: his position there.
	inline void UpdateTarget(const float a_cam[16], const double a_hero[3], bool a_onGotham)
	{
		Target best;
		if (g_enabled.load(std::memory_order_relaxed) && a_onGotham && !g_edges.empty()) {
			double c[3] = { a_cam[12], a_cam[13], a_cam[14] };
			double fw[3] = { a_cam[8], a_cam[9], a_cam[10] };
			double fl = std::sqrt(Dot(fw, fw));
			if (fl > 1e-3) {
				for (double& v : fw) v /= fl;
				const double maxAng = g_maxAngleDeg.load() * coords::kPi / 180.0, minD = g_minDist.load(), maxD = g_maxDist.load();
				const double inset = g_inset.load(), lift = g_lift.load();
				int          prev = g_target.valid ? g_target.edge : -1;
				Target       prevT;
				for (size_t i = 0; i < g_edges.size(); ++i) {
					const Edge& e = g_edges[i];
					double      a[3], b[3];
					for (int k = 0; k < 3; ++k) a[k] = gotham::Placed(static_cast<float>(e.a[k]), k), b[k] = gotham::Placed(static_cast<float>(e.b[k]), k);
					double mid[3] = { (a[0] + b[0]) * 0.5 - a_hero[0], (a[1] + b[1]) * 0.5 - a_hero[1], (a[2] + b[2]) * 0.5 - a_hero[2] };
					double d1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
					double len2 = Dot(d1, d1), len = std::sqrt(len2);
					if (std::sqrt(Dot(mid, mid)) > maxD + len * 0.5 + 5.0) continue;
					// the point of the ledge nearest the camera's line of sight
					double r[3] = { a[0] - c[0], a[1] - c[1], a[2] - c[2] };
					double s = 0.0;
					if (len2 > 1e-6) {
						double bb = Dot(d1, fw), cc = Dot(d1, r), ff = Dot(fw, r), den = len2 - bb * bb;
						s = den > 1e-9 ? (bb * ff - cc) / den : 0.0;
						s = std::fmin(std::fmax(s, 0.0), 1.0);
						double t = bb * s + ff;
						if (t < 0.0) s = std::fmin(std::fmax(-cc / len2, 0.0), 1.0);
						double m = len > 0.8 ? 0.4 / len : 0.5;  // not right at the ends
						s = std::fmin(std::fmax(s, m), 1.0 - m);
					}
					double hn = std::sqrt(static_cast<double>(e.n[0]) * e.n[0] + static_cast<double>(e.n[2]) * e.n[2]);
					double nx = hn > 1e-3 ? e.n[0] / hn : 0.0, nz = hn > 1e-3 ? e.n[2] / hn : 0.0;  // outward, horizontal
					double p[3] = { a[0] + d1[0] * s - nx * inset, a[1] + d1[1] * s + lift, a[2] + d1[2] * s - nz * inset };
					double v[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
					double vl = std::sqrt(Dot(v, v));
					if (vl < 1e-3) continue;
					double cosA = Dot(v, fw) / vl;
					if (cosA <= 0.0) continue;
					double ang = std::acos(std::fmin(cosA, 1.0));
					double h[3] = { p[0] - a_hero[0], p[1] - a_hero[1], p[2] - a_hero[2] };
					double dist = std::sqrt(Dot(h, h));
					if (ang > maxAng || dist < minD || dist > maxD) continue;
					double toCam[3] = { c[0] - p[0], 0.0, c[2] - p[2] };
					float  score = static_cast<float>(ang + 0.0015 * dist + (p[1] < a_hero[1] - 3.0 ? 0.08 : 0.0) + (nx * toCam[0] + nz * toCam[2] < 0.0 ? 0.04 : 0.0));
					Target t;
					t.valid = true;
					for (int k = 0; k < 3; ++k) t.pos[k] = p[k], t.abs[k] = p[k] - gotham::g_place[k] + gotham::g_anchorGuest[k];
					int    mode = g_dirMode.load();
					double hx = h[0], hz = h[2], hl = std::hypot(hx, hz);
					if (mode == 1 && hl > 1e-3) t.dir[0] = static_cast<float>(hx / hl), t.dir[2] = static_cast<float>(hz / hl);
					if (mode == 2) t.dir[0] = static_cast<float>(nx), t.dir[2] = static_cast<float>(nz);
					if (mode == 3) t.dir[0] = static_cast<float>(-nx), t.dir[2] = static_cast<float>(-nz);
					t.edge = static_cast<int>(i);
					t.score = score;
					t.dist = dist;
					if (!best.valid || score < best.score) best = t;
					if (static_cast<int>(i) == prev) prevT = t;
				}
				// keep the ledge already shown unless another is clearly better (no flicker between neighbours)
				if (prevT.valid && best.valid && prevT.score <= best.score * 1.25f + 0.01f) best = prevT;
			}
		}
		AcquireSRWLockExclusive(&g_lock);
		g_target = best;
		ReleaseSRWLockExclusive(&g_lock);
	}

	inline Target Current()
	{
		AcquireSRWLockShared(&g_lock);
		Target t = g_target;
		ReleaseSRWLockShared(&g_lock);
		return t;
	}

	// A point launch to a_t on the next chance (slot 88 or the pump).
	inline void Request(const Target& a_t)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_request = a_t;
		g_requestAt = GetTickCount64();
		g_pending.store(true, std::memory_order_release);
		ReleaseSRWLockExclusive(&g_lock);
	}

	// L2 + R2 pressed together (the edge) with a target asks for the launch. Called where the game reads
	// its controller (the virtual pad), so the request is waiting before the hero states see the press.
	// One press, one launch: "pressed" needs both triggers past 0x50 and ends only when one drops under
	// 0x28 (a trigger hovering at the threshold made two launches, the second restarting the zip), and no
	// new launch is asked for within 0.8 s of the last.
	inline void OnPad(uint8_t a_lt, uint8_t a_rt, uint16_t a_buttons)
	{
		static std::atomic<bool> s_a{ false }, s_b{ false };
		bool                     a = (a_buttons & 0x1000) != 0, b = (a_buttons & 0x2000) != 0;  // XINPUT_GAMEPAD_A, _B
		if (a && !s_a.exchange(true)) g_aPressedAt = GetTickCount64();
		if (!a) s_a = false;
		if (b && !s_b.exchange(true)) g_bPressedAt = GetTickCount64();
		if (!b) s_b = false;
		static std::atomic<bool> s_held{ false };
		bool                     held = s_held.load(std::memory_order_relaxed);
		if (held) {
			if (a_lt < 0x28 || a_rt < 0x28) s_held = false;
			return;
		}
		if (a_lt <= 0x50 || a_rt <= 0x50 || s_held.exchange(true)) return;
		if (GetTickCount64() - g_requestAt.load() < 800) return;
		Target t = Current();
		if (t.valid) Request(t);
	}

	// Worker thread: keeps g_tm current (a respawn or a load makes a new hero).
	inline void KeepTm(bool a_inWorld)
	{
		if (!g_ready.load() || !a_inWorld) return;
		uintptr_t tm = g_tm.load();
		if (tm && ValidTm(tm)) return;
		if (tm) g_tm = 0;
		ULONGLONG now = GetTickCount64();
		if (g_tmScanning.load() || now - g_lastTmScan < 30000) return;
		g_lastTmScan = now;
		g_tmScanning = true;
		if (HANDLE h = CreateThread(nullptr, 0, TmScanThread, nullptr, 0, nullptr)) CloseHandle(h);
		else g_tmScanning = false;
	}

	inline uint32_t Flags()
	{
		Target   t = Current();
		uint32_t f = (t.valid ? 1u : 0u) | (GetTickCount64() < g_activeUntil.load() ? 2u : 0u) | (g_ready.load() && g_tm.load() ? 4u : 0u);
		return f;
	}

	// ---- `zip ...` -----------------------------------------------------------------------------------------
	// zip status | on | off | go | ahead <m> | angle <deg> | range <min> <max> | inset <m> | lift <m>
	//     | dir none|travel|out|in | fallback on|off [ms] | flag <offset hex 70..83> <value> | flag clear | findtm
	inline void Command(const char* a_verb, const char* a_a, const char* a_b, const float* a_cam, bool a_haveCam)
	{
		std::string v = a_verb ? a_verb : "status";
		if (v == "on" || v == "off") {
			g_enabled = v == "on";
		} else if (v == "go") {
			Target t = Current();
			if (t.valid) Request(t);
			else Log("  no target");
		} else if (v == "ahead" && a_haveCam) {
			// a point straight ahead of the camera, for trying the launch without a ledge
			double m = a_a ? atof(a_a) : 20.0;
			Target t;
			t.valid = true;
			for (int k = 0; k < 3; ++k) {
				t.pos[k] = a_cam[12 + k] + a_cam[8 + k] * m;
				t.abs[k] = t.pos[k] - gotham::g_place[k] + gotham::g_anchorGuest[k];
			}
			double hl = std::hypot(a_cam[8], a_cam[10]);
			if (hl > 1e-3) t.dir[0] = static_cast<float>(a_cam[8] / hl), t.dir[2] = static_cast<float>(a_cam[10] / hl);
			t.dist = m;
			Request(t);
		} else if (v == "angle" && a_a) {
			g_maxAngleDeg = static_cast<float>(atof(a_a));
		} else if (v == "range" && a_a && a_b) {
			g_minDist = static_cast<float>(atof(a_a)), g_maxDist = static_cast<float>(atof(a_b));
		} else if (v == "inset" && a_a) {
			g_inset = static_cast<float>(atof(a_a));
		} else if (v == "lift" && a_a) {
			g_lift = static_cast<float>(atof(a_a));
		} else if (v == "dir" && a_a) {
			std::string m = a_a;
			g_dirMode = m == "none" ? 0 : m == "out" ? 2 : m == "in" ? 3 : 1;
		} else if (v == "fallback" && a_a) {
			g_fallback = !_stricmp(a_a, "on");
			if (a_b) g_fallbackMs = atoi(a_b);
		} else if (v == "flag" && a_a && !_stricmp(a_a, "clear")) {
			for (auto& f : g_flagOverride) f = -1;
			g_flagsSet = false;
		} else if (v == "flag" && a_a && a_b) {
			int off = static_cast<int>(strtol(a_a, nullptr, 16));
			if (off >= 0x70 && off < 0x84) {
				g_flagOverride[off - 0x70] = static_cast<int16_t>(strtol(a_b, nullptr, 0) & 0xff);
				g_flagsSet = true;
			}
		} else if (v == "perch" && a_a) {
			g_perch = !_stricmp(a_a, "on");
			if (!g_perch) g_holdSince = 0, g_perchedAt = 0;
		} else if (v == "findtm") {
			g_lastTmScan = 0;
			g_tm = 0;
		} else if (v != "status") {
			Log("  usage: zip status | on | off | go | ahead <m> | angle <deg> | range <min> <max> | inset <m> | lift <m> | dir none|travel|out|in | "
				"fallback on|off [ms] | perch on|off | flag <70..83> <value> | flag clear | findtm");
			return;
		}
		Target t = Current();
		std::string flags;
		for (int i = 0; i < 0x14; ++i) {
			if (g_flagOverride[i] >= 0) {
				char b[24];
				snprintf(b, sizeof(b), " +%02x=%d", 0x70 + i, g_flagOverride[i]);
				flags += b;
			}
		}
		Log("  zip %s, %s; %d ledges; angle %.0f deg, range %.0f..%.0f m, inset %.2f, lift %.2f, dir %d, fallback %s %d ms; transition manager %p; "
			"started %u, refused %u (slot 88 %u, pump %u); target %s%s",
			g_enabled ? "on" : "off", g_ready ? "hooked" : "NOT hooked", g_edgeCount.load(), g_maxAngleDeg.load(), g_minDist.load(), g_maxDist.load(),
			g_inset.load(), g_lift.load(), g_dirMode.load(), g_fallback ? "on" : "off", g_fallbackMs.load(), reinterpret_cast<void*>(g_tm.load()),
			g_started.load(), g_failed.load(), g_viaSlot.load(), g_viaPump.load(), t.valid ? "yes" : "none", flags.empty() ? "" : (" flags" + flags).c_str());
		if (t.valid) Log("  target (%.2f %.2f %.2f), %.1f m away, ledge %d", t.pos[0], t.pos[1], t.pos[2], t.dist, t.edge);
		Log("  perch %s: %u perched, %u launched with A%s%s", g_perch ? "on" : "off", g_perches.load(), g_launches.load(),
			g_perchedAt.load() ? "; perched now" : g_holdSince.load() ? "; holding the zip's launch" : "",
			g_pinState.load() == 2 ? " (pinned)" : g_pinState.load() == 1 ? " (finding the pin)" : "");
	}
}
