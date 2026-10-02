// ArkWeb host: Batman: Arkham Knight side (dinput8.dll proxy).
//
// Phase 1: links to the guest through shared memory, publishes where Batman is, and (with
// DrivePuppet=1) moves Batman to wherever the guest's hero is, every game-thread tick. The tick
// is UObject::ProcessEvent seeing the player controller's PlayerTick event.
#include "../common/fwd_dinput8.h"
#include "../common/coords.h"
#include "../common/link.h"
#include "devcmd.h"
#include "ue3.h"

#include <Xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>

using namespace arkweb;
using namespace arkweb::ue3;

namespace
{
	HMODULE g_self = nullptr;
	Link    g_link;

	ProcessEventFn g_processEvent = nullptr;  // trampoline to the original

	// Resolved by the worker once the object system is up.
	std::atomic<int32_t> g_tickName{ -1 };
	uintptr_t            g_fnSetLocation = 0;  // BmGame.RPawn.SetLocationIgnoringCollision (or Actor.SetLocation)
	bool                 g_setLocIgnoresCollision = false;
	uintptr_t            g_fnSetRotation = 0;
	uintptr_t            g_fnSetPhysics = 0;
	uintptr_t            g_fnSetHidden = 0;  // BmGame.RPawnCharacter.SetHidden (Actor.SetHidden's native)
	std::atomic<bool>    g_ready{ false };
	int32_t              g_playerControllerName = -1;
	uintptr_t            g_lastController = 0;

	// Dev commands run on the game thread. While Arkham is paused because it lost focus (no
	// PlayerTick for 2 s and its window isn't in front) the worker runs them instead: nothing
	// simulates or streams then, so reads can't race. A loading screen also stops ticks but keeps
	// the window in front, so it never qualifies.
	SRWLOCK               g_cmdLock = SRWLOCK_INIT;
	std::atomic<uint64_t> g_lastTickMs{ 0 };
	ue3::FVector          g_lastBatman{};

	void PollCommands(const ue3::FVector& a_batman)
	{
		if (!TryAcquireSRWLockExclusive(&g_cmdLock)) return;
		devcmd::Poll(a_batman);
		ReleaseSRWLockExclusive(&g_cmdLock);
	}

	// The streamer's commands (tile scans, PhysX exports) come through the host ring, read every tick.
	Ring              g_hostRing;
	std::atomic<bool> g_hostRingReady{ false };

	void DrainHostRing(const ue3::FVector& a_batman)
	{
		if (!g_hostRingReady.load(std::memory_order_acquire) || !TryAcquireSRWLockExclusive(&g_cmdLock)) return;
		g_hostRing.Drain(
			[&](uint32_t a_type, const uint8_t* a_p, uint32_t a_bytes) {
				if (a_type != proto::kRecCommand) return;
				std::string line(reinterpret_cast<const char*>(a_p), a_bytes);
				devcmd::RunLine(line, a_batman, line.rfind("tscan", 0) == 0);  // tile scans are many: not logged one by one
			},
			32);
		ReleaseSRWLockExclusive(&g_cmdLock);
	}

	bool ArkhamInFront()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	// Diagnostics: which events fire, for a few seconds after resolving (in case PlayerTick isn't one).
	std::atomic<uint32_t>* g_eventCounts = nullptr;
	constexpr int          kMaxNames = 1 << 20;
	std::atomic<bool>      g_counting{ false };

	// Game-thread state
	bool     g_driving = false;
	uint64_t g_frame = 0;
	int      g_drivePuppet = 1;
	uint64_t g_lastGuestFrame = 0;

	void Call(uintptr_t a_obj, uintptr_t a_fn, void* a_parms)
	{
		if (a_obj && a_fn) g_processEvent(reinterpret_cast<void*>(a_obj), reinterpret_cast<void*>(a_fn), a_parms, nullptr);
	}

	void SetPhysics(uintptr_t a_pawn, uint8_t a_phys)
	{
		struct
		{
			uint8_t  phys;
			uint8_t  pad[3];
			uint32_t wake;
		} p{ a_phys, {}, 1 };
		Call(a_pawn, g_fnSetPhysics, &p);
	}

	void SetLocation(uintptr_t a_pawn, FVector a_loc)
	{
		struct
		{
			FVector  loc;
			uint32_t ret;
		} p{ a_loc, 0 };
		Call(a_pawn, g_fnSetLocation, &p);
	}

	void SetRotation(uintptr_t a_pawn, FRotator a_rot)
	{
		struct
		{
			FRotator rot;
			uint32_t ret;
		} p{ a_rot, 0 };
		Call(a_pawn, g_fnSetRotation, &p);
	}

	// Batman's model (and with it his cape and shadow) out of the picture while Spider-Man is drawn
	// in his place (overlay.h); `batman hide|show|auto`.
	bool      g_batmanHidden = false;
	uintptr_t g_hiddenPawn = 0;
	uint32_t  g_rehides = 0;

	// Arkham's own third-person camera keeps running under the view override and hides the pawn when it
	// gets too close, then shows it again (R3rdPersonCamera.PawnHiddenByCamera) - which undid our hide:
	// Batman reappeared until the next focus change. So while he should be hidden, Actor.bHidden (+0x100,
	// mask 0x20) is checked every tick and set again whenever something cleared it.
	constexpr int      kActorHiddenBits = 0x100;
	constexpr uint32_t kActorHiddenMask = 0x20;

	void UpdateBatmanHidden(uintptr_t a_pawn, bool a_driving)
	{
		bool drawing = GetTickCount64() - overlay::g_lastDrawMs.load(std::memory_order_relaxed) < 500;
		bool want = devcmd::g_batmanHide == 1 || (devcmd::g_batmanHide < 0 && a_driving && drawing);
		if (!g_fnSetHidden) return;
		if (want == g_batmanHidden && a_pawn == g_hiddenPawn) {
			if (!want || (ReadOr<uint32_t>(a_pawn + kActorHiddenBits, kActorHiddenMask) & kActorHiddenMask)) return;
			uint32_t p = 1u;  // something showed him again
			Call(a_pawn, g_fnSetHidden, &p);
			if (++g_rehides <= 5 || g_rehides % 100 == 0) Log("Batman's model was shown by Arkham (its camera?) - hidden again (%u times)", g_rehides);
			return;
		}
		uint32_t p = want ? 1u : 0u;
		Call(a_pawn, g_fnSetHidden, &p);
		Log("Batman's model %s", want ? "hidden (Spider-Man is drawn in his place)" : "shown");
		g_batmanHidden = want;
		g_hiddenPawn = a_pawn;
	}

	// ---- collision scan (devcmd "scan"): downward Actor.Trace columns on a grid around Batman ----------
	constexpr int kScanColumnsPerTick = 60;

	// Traces call UWorld::SingleLineCheck (exe+ac7f60) directly, with the arguments AActor::execTrace
	// (exe+9c2be0) passes it. Going through ProcessEvent + Engine.Actor.Trace returned nothing, not even
	// for a 5 m trace through the floor under Batman (its out parameters never reached our parms).
	//   SingleLineCheck(GWorld, FCheckResult& hit, AActor* ignore, const FVector& end, const FVector& start,
	//                   int filter, DWORD flags, const FVector& extent, void* = 0)
	// FCheckResult (0x78): +0x08 Actor, +0x10 Location, +0x1c Normal, +0x28 Time.
	constexpr uintptr_t kSingleLineCheck = 0xac7f60, kGWorld = 0x311c750;
	constexpr uint8_t   kSingleLineCheckSig[18] = { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8b, 0xec, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00 };
	using SingleLineCheckFn = uint32_t (*)(void* a_world, void* a_hit, void* a_ignore, const FVector* a_end, const FVector* a_start, int a_filter,
		uint32_t a_flags, const FVector* a_extent, void* a_unused);
	SingleLineCheckFn g_singleLineCheck = nullptr;

	bool Trace(uintptr_t a_pawn, const FVector& a_start, const FVector& a_end, uint8_t a_filter, FVector& a_hit, FVector& a_normal, float a_extent = 0.0f)
	{
		if (!g_singleLineCheck) return false;
		void* world = reinterpret_cast<void*>(ReadOr<uintptr_t>(g_exe + kGWorld, 0));
		if (!world) return false;
		alignas(16) uint8_t hit[0x80] = {};
		const float one = 1.0f;
		const int32_t none = -1;
		const uint64_t six = 6;
		memcpy(hit + 0x28, &one, 4), memcpy(hit + 0x2c, &none, 4), memcpy(hit + 0x60, &none, 4);
		memcpy(hit + 0x68, &one, 4), memcpy(hit + 0x6c, &none, 4), memcpy(hit + 0x70, &six, 8);
		// A zero extent is a line trace - and many Gotham street meshes don't block those (UE3
		// BlockZeroExtent off: only the pawn's cylinder collides). A small box sweep hits them.
		FVector extent{ a_extent, a_extent, a_extent };
		g_singleLineCheck(world, hit, reinterpret_cast<void*>(a_pawn), &a_end, &a_start, a_filter, 0, &extent, nullptr);
		uint64_t actor = 0;
		float    time = 1.0f;
		memcpy(&actor, hit + 0x08, 8);
		memcpy(&a_hit, hit + 0x10, sizeof(FVector));
		memcpy(&a_normal, hit + 0x1c, sizeof(FVector));
		memcpy(&time, hit + 0x28, 4);
		a_hit.z -= a_extent;  // a box sweep reports the box center at contact; the surface is under it
		static int s_logged = 0;
		if (s_logged < 6) {
			++s_logged;
			Log("trace %d (filter %d) from (%.0f %.0f %.0f) to (%.0f %.0f %.0f): hit (%.1f %.1f %.1f) normal (%.3f %.3f %.3f) actor %llx time %.3f",
				s_logged, a_filter, a_start.x, a_start.y, a_start.z, a_end.x, a_end.y, a_end.z, a_hit.x, a_hit.y, a_hit.z, a_normal.x, a_normal.y,
				a_normal.z, static_cast<unsigned long long>(actor), time);
		}
		return time < 1.0f && (actor != 0 || a_normal.x != 0.0f || a_normal.y != 0.0f || a_normal.z != 0.0f);
	}

	// A streamed tile's column (traces cost about 0.3 ms each, so as few as possible): down from the top;
	// after each surface the next trace starts 4 m below it. If that start is inside a solid (time 0),
	// the surface was a roof or the ground itself and the column ends - the streamer extends streets
	// under buildings and takes building walls from PhysX. Otherwise it was a deck (a bridge, an
	// overpass) and the street below is found too. A top inside a solid steps down 10 m at a time.
	void ScanTileColumn(uintptr_t a_pawn, devcmd::ScanJob& a_j, int a_i, int a_jj, float a_x, float a_y, float a_top, float a_bottom)
	{
		float z = a_top;
		int   layers = 0;
		for (int it = 0; it < 16 && layers < 8 && z > a_bottom; ++it) {
			FVector hit{}, n{};
			++a_j.traces;
			if (!Trace(a_pawn, { a_x, a_y, z }, { a_x, a_y, a_bottom }, a_j.filter, hit, n, devcmd::g_scanExtent)) break;
			if (hit.z > z - devcmd::g_scanExtent - 2.0f) {  // started inside a solid
				if (layers) break;
				z -= 1000.0f;
				continue;
			}
			++a_j.hits, ++layers;
			fprintf(a_j.out, "%d %d %.1f %.1f %.1f %.3f %.3f %.3f\n", a_i, a_jj, hit.x, hit.y, hit.z, n.x, n.y, n.z);
			z = hit.z - 400.0f;
		}
	}

	// Each column traces down from 200 m above Batman to 300 m below, then again from just under each
	// hit, so stacked surfaces (roofs, ledges, streets) are all recorded.
	void StepScan(uintptr_t a_pawn)
	{
		for (int r = 0; r < devcmd::g_traceReqCount && g_singleLineCheck; ++r) {
			auto&   q = devcmd::g_traceReqs[r];
			FVector hit{}, n{};
			bool    got = Trace(a_pawn, q.start, q.end, q.filter, hit, n);
			Log("trace (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) filter %d: %s at (%.1f %.1f %.1f) normal (%.3f %.3f %.3f)", q.start.x, q.start.y, q.start.z,
				q.end.x, q.end.y, q.end.z, q.filter, got ? "HIT" : "no hit", hit.x, hit.y, hit.z, n.x, n.y, n.z);
		}
		devcmd::g_traceReqCount = 0;
		auto& j = devcmd::g_scan;
		if (!j.active && !devcmd::g_scanQueue.empty()) {
			j = std::move(devcmd::g_scanQueue.front());
			devcmd::g_scanQueue.pop_front();
			j.out = _wfopen(j.part.c_str(), L"w");
			if (!j.out) {
				Log("tscan %s: can't write %ls", j.name.c_str(), j.part.c_str());
				return;
			}
			fprintf(j.out, "# ArkWeb tile scan, UU, Z up. rect %.1f %.1f, columns %d x %d, step %.1f, top %.0f, bottom %.0f, filter %d\n", j.x0, j.y0,
				j.nx, j.ny, j.stepUU, j.topUU, j.bottomUU, j.filter);
			j.startMs = GetTickCount64();
			j.active = true;
		}
		if (!j.active) return;
		if (!g_singleLineCheck) {
			Log("scan: SingleLineCheck not available, aborted");
			fclose(j.out);
			j.active = false;
			devcmd::g_scanQueue.clear();
			return;
		}
		const float top = j.tile ? j.topUU : j.center.z + j.topUU, bottom = j.tile ? j.bottomUU : j.center.z + j.bottomUU;
		const int   total = j.tile ? j.nx * j.ny : j.n * j.n;
		LARGE_INTEGER t0, now, freq;
		QueryPerformanceCounter(&t0);
		QueryPerformanceFrequency(&freq);
		const LONGLONG budget = static_cast<LONGLONG>(devcmd::g_scanBudgetMs * freq.QuadPart / 1000.0);
		for (int k = 0; j.next < total; ++j.next) {
			if (k >= 8 && (k & 3) == 0) {  // at least 8 columns, then until the tick's time budget is used
				QueryPerformanceCounter(&now);
				if (now.QuadPart - t0.QuadPart > budget || (!j.tile && k >= kScanColumnsPerTick)) break;
			}
			int   i, jj;
			float x, y;
			if (j.tile) {
				i = j.next % j.nx, jj = j.next / j.nx;
				x = j.x0 + i * j.stepUU, y = j.y0 + jj * j.stepUU;
			} else {
				i = j.next % j.n, jj = j.next / j.n;
				x = j.center.x - j.radiusUU + i * j.stepUU, y = j.center.y - j.radiusUU + jj * j.stepUU;
				float dx = x - j.center.x, dy = y - j.center.y;
				if (dx * dx + dy * dy > j.radiusUU * j.radiusUU) continue;
			}
			++k;
			if (j.tile) {
				ScanTileColumn(a_pawn, j, i, jj, x, y, top, bottom);
				continue;
			}
			// A trace that starts inside a solid hits at its own start (z unchanged): that is not a
			// surface, so step 50 UU further down through the solid, giving up after 20 m (a building).
			float z = top;
			int   layers = 0, inside = 0;
			for (int it = 0; it < 80 && layers < 12 && z > bottom; ++it) {
				FVector hit{}, n{};
				++j.traces;
				if (!Trace(a_pawn, { x, y, z }, { x, y, bottom }, j.filter, hit, n)) break;
				if (hit.z > z - 2.0f) {
					if (++inside > 40) break;
					z -= 50.0f;
					continue;
				}
				inside = 0;
				++j.hits, ++layers;
				fprintf(j.out, "%d %d %.1f %.1f %.1f %.3f %.3f %.3f\n", i, jj, hit.x, hit.y, hit.z, n.x, n.y, n.z);
				z = hit.z - 20.0f;
			}
			if (!j.tile && j.next % 4000 == 0) Log("scan: column %d / %d, %llu traces, %llu hits", j.next, total, j.traces, j.hits);
		}
		if (j.next >= total) {
			fclose(j.out);
			j.active = false;
			if (j.tile) {
				MoveFileExW(j.part.c_str(), j.final.c_str(), MOVEFILE_REPLACE_EXISTING);
				static uint32_t s_tiles = 0;
				if (++s_tiles % 25 == 1) Log("tscan %s done (%u tiles so far): %llu traces, %llu hits in %.2f s, %zu queued", j.name.c_str(), s_tiles,
					j.traces, j.hits, (GetTickCount64() - j.startMs) / 1000.0, devcmd::g_scanQueue.size());
			} else {
				Log("scan done: %llu traces, %llu hits in %.1f s", j.traces, j.hits, (GetTickCount64() - j.startMs) / 1000.0);
			}
		}
	}

	// Is a_obj's class (or a superclass) named PlayerController? UStruct::SuperField is at +0x5C.
	bool IsPlayerController(uintptr_t a_obj)
	{
		if (a_obj == g_lastController) return true;
		for (uintptr_t c = ClassOf(a_obj), d = 0; c && d < 32; c = ReadOr<uintptr_t>(c + 0x5C, 0), ++d) {
			if (NameIndexOf(c) == g_playerControllerName) {
				g_lastController = a_obj;
				return true;
			}
		}
		return false;
	}

	// ---- player input -> the guest's virtual pad (while Batman is the puppet) ---------------------------
	// A connected controller is forwarded whole (the guest serves it as Spider-Man's XInput pad);
	// otherwise, while Arkham is in front, the keyboard: WASD = left stick, Space = jump (A), Left Shift
	// = swing (right trigger), Left Ctrl = dodge (B) - Spider-Man's own defaults. The camera direction
	// goes along so the guest can turn Spider-Man's camera the same way (movement is camera relative).
	constexpr int kPlayerCamera = 0x478;  // Engine.PlayerController.PlayerCamera
	constexpr int kCameraPovRotation = 0x580;  // Camera.CameraCache.POV.Rotation (pitch, yaw, roll)
	constexpr int kCameraPovFov = 0x58C;       // Camera.CameraCache.POV.FOV (degrees, horizontal)
	using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
	XInputGetStateFn g_xinputGetState = nullptr;
	uint32_t         g_padPacket = 0;

	bool ArkhamInFront();

	void PublishPad(uintptr_t a_controller, bool a_active)
	{
		if (!g_xinputGetState) {
			static bool tried = false;
			if (!tried) {
				tried = true;
				for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" }) {
					if (HMODULE m = LoadLibraryW(dll)) {
						g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
						if (g_xinputGetState) {
							Log("pad: XInput from %ls", dll);
							break;
						}
					}
				}
			}
		}
		proto::PadState p{};
		if (a_active) {
			p.flags = proto::kPadActive;
			XINPUT_STATE xs{};
			if (g_xinputGetState && g_xinputGetState(0, &xs) == ERROR_SUCCESS) {
				p.flags |= proto::kPadAnalog;
				p.buttons = xs.Gamepad.wButtons;
				p.leftTrigger = xs.Gamepad.bLeftTrigger, p.rightTrigger = xs.Gamepad.bRightTrigger;
				p.thumbLX = xs.Gamepad.sThumbLX, p.thumbLY = xs.Gamepad.sThumbLY;
				p.thumbRX = xs.Gamepad.sThumbRX, p.thumbRY = xs.Gamepad.sThumbRY;
			} else if (ArkhamInFront()) {
				auto down = [](int a_vk) { return (GetAsyncKeyState(a_vk) & 0x8000) != 0; };
				p.thumbLY = static_cast<int16_t>((down('W') ? 32767 : 0) - (down('S') ? 32767 : 0));
				p.thumbLX = static_cast<int16_t>((down('D') ? 32767 : 0) - (down('A') ? 32767 : 0));
				if (down(VK_SPACE)) p.buttons |= XINPUT_GAMEPAD_A;
				if (down(VK_LCONTROL)) p.buttons |= XINPUT_GAMEPAD_B;
				if (down(VK_LSHIFT)) p.rightTrigger = 255;
			}
			// With a controller and the view mimicking Spider-Man's camera, his camera is the one in charge:
			// the right stick reaches it unchanged (no steering toward Arkham's camera, which isn't shown).
			bool      steer = !(devcmd::g_mimicCamera && (p.flags & proto::kPadAnalog));
			uintptr_t cam = ReadOr<uintptr_t>(a_controller + kPlayerCamera, 0);
			int32_t   rot[3] = {};
			if (steer && cam && SafeRead(rot, reinterpret_cast<const void*>(cam + kCameraPovRotation), sizeof(rot))) {
				coords::V3 g = coords::HostDirToGuest(coords::HostForwardOfYaw(rot[1]));
				p.camYaw = static_cast<float>(coords::GuestYawOfForward(g));
				int pitch = static_cast<int16_t>(rot[0] & 0xFFFF);  // UE3 pitch wraps at 65536
				p.camPitch = static_cast<float>(pitch * coords::kPi / 32768.0);
				p.flags |= proto::kPadCamera;
			}
		}
		static proto::PadState last{};
		if (memcmp(&p.buttons, &last.buttons, 0x18 - 0xC) != 0 || p.flags != last.flags) ++g_padPacket;
		last = p;
		p.packet = g_padPacket;
		SeqWrite(g_link.Pad(), p);
	}

	// ---- Spider-Man's motion, smoothed --------------------------------------------------------------------
	// The guest publishes one snapshot per Spider-Man frame (hero and camera together), stamped with that
	// frame's QPC time - the same clock here. Arkham ticks at its own rate, so each tick samples the motion
	// a little in the past (about one Spider-Man frame plus the publish delay) and interpolates between the
	// two frames around that moment: Batman and the view move smoothly, and together. Past the newest frame
	// (a hitch) it extrapolates for up to 50 ms.
	struct Snap
	{
		int64_t qpc;
		double  pos[3];
		float   fwd[3];
		double  cam[3];
		float   camF[3], camU[3];
		float   fovY;  // Spider-Man's vertical field of view, degrees (0 = not known)
		bool    camOk;
	};
	using Pose = Snap;
	constexpr int kSnaps = 32;
	Snap          g_snaps[kSnaps];
	int           g_snapCount = 0, g_snapNext = 0;
	double        g_frameDt = 1.0 / 60.0;  // average time between Spider-Man frames, s
	double        g_qpcFreq = 0.0;
	FVector       g_viewLoc{};  // this tick's view (OverrideView)
	FRotator      g_viewRot{};
	bool          g_viewValid = false;
	float         g_viewFovY = 0.0f;  // ... and its vertical field of view (0: Arkham's own)

	const Snap& SnapAt(int a_i) { return g_snaps[(g_snapNext - g_snapCount + a_i + 2 * kSnaps) % kSnaps]; }  // 0 = oldest

	void PushSnap(const proto::GuestState& a_gs)
	{
		if (!a_gs.qpc) return;
		if (g_snapCount) {
			const Snap& last = SnapAt(g_snapCount - 1);
			if (a_gs.qpc <= last.qpc) return;  // the same frame again
			double dt = (a_gs.qpc - last.qpc) / g_qpcFreq;
			if (dt > 0.0 && dt < 0.2) g_frameDt = g_frameDt * 0.9 + dt * 0.1;
		}
		Snap& s = g_snaps[g_snapNext];
		g_snapNext = (g_snapNext + 1) % kSnaps;
		if (g_snapCount < kSnaps) ++g_snapCount;
		s.qpc = a_gs.qpc;
		for (int i = 0; i < 3; ++i) {
			s.pos[i] = a_gs.heroPos[i], s.fwd[i] = a_gs.heroRot[6 + i];
			s.cam[i] = a_gs.camPos[i], s.camF[i] = a_gs.camRot[6 + i], s.camU[i] = a_gs.camRot[3 + i];
		}
		s.camOk = s.camF[0] * s.camF[0] + s.camF[1] * s.camF[1] + s.camF[2] * s.camF[2] > 0.5f;
		s.fovY = a_gs.camFovYDeg;
	}

	void Normalize(float a_v[3])
	{
		float l = std::sqrt(a_v[0] * a_v[0] + a_v[1] * a_v[1] + a_v[2] * a_v[2]);
		if (l > 1e-6f) a_v[0] /= l, a_v[1] /= l, a_v[2] /= l;
	}

	void Blend(const Snap& a_a, const Snap& a_b, double a_t, Pose& a_out)
	{
		const float t = static_cast<float>(a_t);
		for (int i = 0; i < 3; ++i) {
			a_out.pos[i] = a_a.pos[i] + (a_b.pos[i] - a_a.pos[i]) * a_t;
			a_out.cam[i] = a_a.cam[i] + (a_b.cam[i] - a_a.cam[i]) * a_t;
			a_out.fwd[i] = a_a.fwd[i] + (a_b.fwd[i] - a_a.fwd[i]) * t;
			a_out.camF[i] = a_a.camF[i] + (a_b.camF[i] - a_a.camF[i]) * t;
			a_out.camU[i] = a_a.camU[i] + (a_b.camU[i] - a_a.camU[i]) * t;
		}
		Normalize(a_out.fwd), Normalize(a_out.camF), Normalize(a_out.camU);
		a_out.camOk = a_a.camOk && a_b.camOk;
		a_out.fovY = a_a.fovY > 0.0f && a_b.fovY > 0.0f ? a_a.fovY + (a_b.fovY - a_a.fovY) * t : a_b.fovY;
		a_out.qpc = a_a.qpc + static_cast<int64_t>((a_b.qpc - a_a.qpc) * a_t);
	}

	bool SamplePose(Pose& a_out)
	{
		if (!g_snapCount) return false;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		double      delay = std::clamp(g_frameDt + 0.008, 0.010, 0.060);
		int64_t     t = now.QuadPart - static_cast<int64_t>(delay * g_qpcFreq);
		const Snap& newest = SnapAt(g_snapCount - 1);
		if (t >= newest.qpc) {
			a_out = newest;
			if (g_snapCount < 2) return true;
			const Snap& prev = SnapAt(g_snapCount - 2);
			double span = (newest.qpc - prev.qpc) / g_qpcFreq;
			double ahead = std::min((t - newest.qpc) / g_qpcFreq, 0.05);
			double step = std::hypot(newest.pos[0] - prev.pos[0], newest.pos[1] - prev.pos[1], newest.pos[2] - prev.pos[2]);
			if (span > 0.0 && span < 0.2 && step < 20.0) Blend(prev, newest, 1.0 + ahead / span, a_out);  // not across a teleport
			return true;
		}
		for (int i = g_snapCount - 2; i >= 0; --i) {
			const Snap& a = SnapAt(i);
			const Snap& b = SnapAt(i + 1);
			if (a.qpc <= t) {
				Blend(a, b, static_cast<double>(t - a.qpc) / static_cast<double>(b.qpc - a.qpc), a_out);
				return true;
			}
		}
		a_out = SnapAt(0);  // older than everything we have
		return true;
	}

	// One game-thread tick (after the player controller's own PlayerTick ran).
	void OnPlayerTick(uintptr_t a_controller, float a_dt)
	{
		if (!IsPlayerController(a_controller)) return;
		uintptr_t pawn = ReadOr<uintptr_t>(a_controller + kControllerPawn, 0);
		if (!pawn || !g_link.Valid()) return;
		++g_frame;

		FVector  loc = ReadOr<FVector>(pawn + kActorLocation, {});
		FRotator rot = ReadOr<FRotator>(pawn + kActorRotation, {});
		float    halfHeight = ReadOr<float>(ReadOr<uintptr_t>(pawn + kPawnCylinder, 0) + kCylinderHeight, 95.0f);

		// Batman's feet -> guest space for the HostState.
		coords::V3 feet = coords::HostPosToGuest({ loc.x, loc.y, loc.z - halfHeight });
		coords::V3 hfwd = coords::HostForwardOfYaw(rot.yaw);
		coords::V3 gfwd = coords::HostDirToGuest(hfwd);

		// A torn read (the guest was mid-write every try) keeps last tick's state; only the
		// heartbeat or the guest's own InWorld flag ends the link.
		static proto::GuestState gs{};
		proto::GuestState        fresh;
		if (SeqRead(g_link.Guest(), fresh, 64)) gs = fresh;
		// only a guest that stands in Gotham (anchored) drives Batman; before that its position is New York's
		bool haveGuest = Link::Alive(g_link.Header()->guestHeartbeatMs) && (gs.flags & proto::kGuestInWorld) && (gs.flags & proto::kGuestAnchored);

		if (haveGuest && g_drivePuppet) {
			if (!g_driving) {
				Log("guest linked: driving Batman (guest frame %llu)", static_cast<unsigned long long>(gs.frame));
				SetPhysics(pawn, PHYS_None);
				g_driving = true;
				g_snapCount = g_snapNext = 0;  // no history from before this link
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				g_qpcFreq = static_cast<double>(f.QuadPart);
			}
			PushSnap(gs);
			Pose pose;
			if (SamplePose(pose)) {
				coords::V3 h = coords::GuestPosToHost({ pose.pos[0], pose.pos[1], pose.pos[2] });
				// Never follow the guest below the city: Arkham autosaves wherever Batman is, and a save
				// under the map spawns him there for good. Batman waits at his last spot instead.
				constexpr double kLowestPuppetZ = -800.0;  // UU; the sea is at about -270
				if (h.z < kLowestPuppetZ) {
					static ULONGLONG s_warned = 0;
					if (GetTickCount64() - s_warned > 5000) {
						s_warned = GetTickCount64();
						Log("guest is below the city (z %.0f UU): Batman holds his position", h.z);
					}
				} else {
					SetLocation(pawn, { static_cast<float>(h.x), static_cast<float>(h.y), static_cast<float>(h.z + halfHeight) });
				}
				SetRotation(pawn, { 0, coords::GuestForwardToHostYaw({ pose.fwd[0], pose.fwd[1], pose.fwd[2] }), 0 });
				// the view for this frame, from the same moment as Batman
				if (pose.camOk && h.z >= kLowestPuppetZ) {
					coords::V3          c = coords::GuestPosToHost({ pose.cam[0], pose.cam[1], pose.cam[2] });
					coords::HostRotator r = coords::GuestBasisToHostRotator({ pose.camF[0], pose.camF[1], pose.camF[2] }, { pose.camU[0], pose.camU[1], pose.camU[2] });
					g_viewLoc = { static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z) };
					g_viewRot = { r.pitch, r.yaw, r.roll };
					g_viewValid = true;
					g_viewFovY = pose.fovY;
				} else {
					g_viewValid = false;
				}
			}
		} else if (g_driving) {
			Log("guest gone: Batman released");
			SetPhysics(pawn, PHYS_Falling);
			g_driving = false;
		}
		overlay::g_puppet.store(g_driving, std::memory_order_relaxed);
		UpdateBatmanHidden(pawn, g_driving);

		PublishPad(a_controller, g_driving);

		proto::HostState hs{};
		hs.flags = proto::kHostInGame | (g_driving ? proto::kHostDrivePuppet : 0);
		hs.frame = g_frame;
		hs.deltaTime = a_dt;
		hs.puppetPos[0] = feet.x, hs.puppetPos[1] = feet.y, hs.puppetPos[2] = feet.z;
		hs.puppetYaw = static_cast<float>(coords::GuestYawOfForward(gfwd));
		SeqWrite(g_link.Host(), hs);
		g_link.Header()->hostHeartbeatMs = GetTickCount64();

		g_lastBatman = loc;
		g_lastTickMs = GetTickCount64();
		if (g_frame % 15 == 0) PollCommands(loc);  // manual commands; the streamer uses the host ring
		DrainHostRing(loc);
		if (devcmd::g_tpPending) {
			devcmd::g_tpPending = false;
			const FVector& f = devcmd::g_tpFeet;
			SetLocation(pawn, { f.x, f.y, f.z + halfHeight });
			SetPhysics(pawn, PHYS_Falling);
			Log("tp: Batman put at feet (%.0f %.0f %.0f)", f.x, f.y, f.z);
		}
		StepScan(pawn);
		devcmd::StepExport();  // a running px bexport, a couple of ms per tick
		grapple::Tick();  // a slice of GObjects: Arkham's grapple ledges for Spider-Man's zip to point

		if (g_frame % 600 == 1) {
			Log("tick %llu: Batman (%.0f %.0f %.0f) UU = guest (%.2f %.2f %.2f) m, guest %s",
				static_cast<unsigned long long>(g_frame), loc.x, loc.y, loc.z, feet.x, feet.y, feet.z, haveGuest ? "linked" : "absent");
		}
	}

	// ---- Spider-Man's camera as Arkham's view -----------------------------------------------------------
	// The renderer (and audio, aiming, the HUD) ask the player controller for the view through the
	// script event GetPlayerViewPoint(out Vector out_Location @0x0, out Rotator out_Rotation @0xC): Arkham's
	// camera computes its answer as usual, then it is replaced by Spider-Man's camera (the guest publishes
	// it with the hero, in Arkham's frame). Only while Batman is the puppet.
	std::atomic<int32_t> g_viewPointName{ -1 }, g_fovAngleName{ -1 };
	uint64_t             g_viewOverrides = 0, g_fovOverrides = 0;

	// Spider-Man's vertical field of view as Arkham's (horizontal, degrees, for this back buffer's shape).
	float HostFov(float a_vertDeg)
	{
		float aspect = overlay::g_aspect.load(std::memory_order_relaxed);
		if (aspect < 0.5f || aspect > 4.0f) aspect = 16.0f / 9.0f;
		return static_cast<float>(2.0 * std::atan(std::tan(a_vertDeg * coords::kPi / 360.0) * aspect) * 180.0 / coords::kPi);
	}

	bool MimicFov() { return devcmd::g_mimicCamera && devcmd::g_mimicFov && g_driving && g_viewValid && g_viewFovY > 5.0f && g_viewFovY < 150.0f; }

	// The script event GetFOVAngle(return float @0): the renderer's field of view.
	void OverrideFov(uintptr_t a_obj, void* a_parms)
	{
		if (!a_parms || a_obj != g_lastController || !MimicFov()) return;
		*static_cast<float*>(a_parms) = HostFov(g_viewFovY);
		if (++g_fovOverrides % 3000 == 1)
			Log("view: Spider-Man's lens %.1f deg vertical = Arkham FOV %.1f (%llu so far)", g_viewFovY, HostFov(g_viewFovY), static_cast<unsigned long long>(g_fovOverrides));
	}

	// The view is the one OnPlayerTick sampled this frame - the same moment Batman was put at - so the
	// two never shake against each other.
	void OverrideView(uintptr_t a_obj, void* a_parms)
	{
		if (!devcmd::g_mimicCamera || !g_driving || !g_viewValid || !a_parms || a_obj != g_lastController) return;
		memcpy(static_cast<uint8_t*>(a_parms) + 0x0, &g_viewLoc, sizeof(g_viewLoc));
		memcpy(static_cast<uint8_t*>(a_parms) + 0xC, &g_viewRot, sizeof(g_viewRot));
		// the field of view this view is drawn with (Camera.CameraCache.POV.FOV): Spider-Man's lens when it is
		// known (also what GetFOVAngle answers), else Arkham's own; the overlay scales the capture by it
		uintptr_t cam = ReadOr<uintptr_t>(a_obj + kPlayerCamera, 0);
		float     fov = ReadOr<float>(cam + kCameraPovFov, 0.0f);
		if (cam && MimicFov()) {
			fov = HostFov(g_viewFovY);
			SafeWrite(reinterpret_cast<void*>(cam + kCameraPovFov), &fov, sizeof(fov));
		}
		if (fov > 10.0f && fov < 170.0f) overlay::g_akFovDeg.store(fov, std::memory_order_relaxed);
		if (++g_viewOverrides % 3000 == 1) {
			Log("view: Spider-Man's camera at (%.0f %.0f %.0f) UU, pitch %d yaw %d roll %d (%llu views so far, Spider-Man frames %.1f ms apart)",
				g_viewLoc.x, g_viewLoc.y, g_viewLoc.z, g_viewRot.pitch, g_viewRot.yaw, g_viewRot.roll, static_cast<unsigned long long>(g_viewOverrides),
				g_frameDt * 1000.0);
		}
	}

	void ProcessEventDetour(void* a_obj, void* a_fn, void* a_parms, void* a_result)
	{
		int32_t name = ReadOr<int32_t>(reinterpret_cast<uintptr_t>(a_fn) + kObjName, -1);
		if (g_counting.load(std::memory_order_relaxed) && name >= 0 && name < kMaxNames) g_eventCounts[name].fetch_add(1, std::memory_order_relaxed);
		g_processEvent(a_obj, a_fn, a_parms, a_result);
		if (name == g_tickName.load(std::memory_order_relaxed) && g_ready.load(std::memory_order_acquire)) {
			OnPlayerTick(reinterpret_cast<uintptr_t>(a_obj), a_parms ? *static_cast<float*>(a_parms) : 0.0f);
		} else if (name == g_viewPointName.load(std::memory_order_relaxed) && g_ready.load(std::memory_order_acquire)) {
			OverrideView(reinterpret_cast<uintptr_t>(a_obj), a_parms);
		} else if (name == g_fovAngleName.load(std::memory_order_relaxed) && g_ready.load(std::memory_order_acquire)) {
			OverrideFov(reinterpret_cast<uintptr_t>(a_obj), a_parms);
		}
	}

	void Resolve()
	{
		while (NameCount() < 50000 || ObjCount() < 100000) Sleep(1000);
		Sleep(3000);
		std::wstring tick = IniString(g_self, L"TickEvent", L"PlayerTick");
		std::string  tickA(tick.begin(), tick.end());
		g_fnSetLocation = FindObject("BmGame.RPawn.SetLocationIgnoringCollision");
		g_setLocIgnoresCollision = g_fnSetLocation != 0;
		if (!g_fnSetLocation) g_fnSetLocation = FindObject("Engine.Actor.SetLocation");
		g_fnSetRotation = FindObject("Engine.Actor.SetRotation");
		g_fnSetPhysics = FindObject("Engine.Actor.SetPhysics");
		g_fnSetHidden = FindObject("BmGame.RPawnCharacter.SetHidden");
		if (!g_fnSetHidden) g_fnSetHidden = FindObject("Engine.Actor.SetHidden");
		Log("hide: SetHidden %p", reinterpret_cast<void*>(g_fnSetHidden));
		// grapple ledges for Spider-Man's zip to point (grapple.h)
		grapple::g_cls[0] = FindObject("BmGame.RGrapplePoint");
		grapple::g_cls[1] = FindObject("BmGame.RRescuePoint");
		grapple::g_cls[2] = FindObject("BmGame.RGrapplePointCollection");
		grapple::g_classesReady.store(grapple::g_cls[0] || grapple::g_cls[2], std::memory_order_release);
		Log("grapple: classes RGrapplePoint %p, RRescuePoint %p, RGrapplePointCollection %p", reinterpret_cast<void*>(grapple::g_cls[0]),
			reinterpret_cast<void*>(grapple::g_cls[1]), reinterpret_cast<void*>(grapple::g_cls[2]));
		if (memcmp(reinterpret_cast<void*>(g_exe + kSingleLineCheck), kSingleLineCheckSig, sizeof(kSingleLineCheckSig)) == 0)
			g_singleLineCheck = reinterpret_cast<SingleLineCheckFn>(g_exe + kSingleLineCheck);
		int32_t t = FindName(tickA.c_str());
		g_playerControllerName = FindName("PlayerController");
		g_viewPointName = FindName("GetPlayerViewPoint");
		g_fovAngleName = FindName("GetFOVAngle");
		devcmd::g_mimicCamera = IniInt(g_self, L"MimicCamera", 1) != 0;
		Log("view: GetPlayerViewPoint = name %d, mimic Spider-Man's camera %s", g_viewPointName.load(), devcmd::g_mimicCamera ? "on" : "off");
		Log("resolved: %d names, %d objects; tick event '%s' = name %d; SetLocation%s %p, SetRotation %p, SetPhysics %p, SingleLineCheck %s",
			NameCount(), ObjCount(), tickA.c_str(), t, g_setLocIgnoresCollision ? "IgnoringCollision" : "",
			reinterpret_cast<void*>(g_fnSetLocation), reinterpret_cast<void*>(g_fnSetRotation), reinterpret_cast<void*>(g_fnSetPhysics),
			g_singleLineCheck ? "ok" : "SIGNATURE MISMATCH");
		g_tickName = t;
		g_ready = t >= 0 && g_fnSetLocation && g_fnSetRotation && g_fnSetPhysics;

		// Event census over 5 s, so a missing tick event is easy to fix from the log.
		g_eventCounts = new std::atomic<uint32_t>[kMaxNames]();
		g_counting = true;
		Sleep(5000);
		g_counting = false;
		std::vector<std::pair<uint32_t, int>> top;
		for (int i = 0; i < kMaxNames; ++i) {
			if (uint32_t c = g_eventCounts[i].load()) top.emplace_back(c, i);
		}
		std::sort(top.rbegin(), top.rend());
		Log("ProcessEvent census (5 s), %zu distinct functions:", top.size());
		for (size_t i = 0; i < top.size() && i < 25; ++i) Log("  %8u  %s", top[i].first, NameOf(top[i].second).c_str());
	}

	DWORD WINAPI Worker(void*)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		while (!g_link.Open()) Sleep(1000);
		g_link.Header()->hostPid = GetCurrentProcessId();
		g_hostRing = Ring(g_link.Base() + proto::kOffHostRing, proto::kHostRingBytes);
		g_hostRing.Discard();  // left over from an earlier Arkham
		g_hostRingReady = true;
		overlay::g_capture = g_link.Capture();
		overlay::g_header = g_link.Header();
		overlay::g_guest = g_link.Guest();
		overlay::g_enabled = IniInt(g_self, L"Overlay", 1) != 0;
		Log("link open (%s), host ring ready", proto::kMappingNameA);
		Resolve();
		overlay::Install();  // Arkham's renderer is up by now
		for (;;) {
			Sleep(500);
			uint64_t last = g_lastTickMs.load();
			if (last && GetTickCount64() - last > 2000 && !ArkhamInFront()) PollCommands(g_lastBatman);
			grapple::WriteIfDirty(devcmd::StreamDir());
		}
	}
}

BOOL APIENTRY DllMain(HMODULE a_self, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_self);
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		if (!wcsstr(exe, L"BatmanAK.exe")) return TRUE;
		g_self = a_self;
		InitLog(a_self, L"ak_host");
		g_drivePuppet = IniInt(a_self, L"DrivePuppet", 1);
		g_processEvent = reinterpret_cast<ProcessEventFn>(
			InlineHook(g_exe + kProcessEvent, kProcessEventSig, sizeof(kProcessEventSig), reinterpret_cast<void*>(&ProcessEventDetour)));
		Log("ArkWeb host loaded (exe %p): ProcessEvent hook %s, DrivePuppet=%d", reinterpret_cast<void*>(g_exe),
			g_processEvent ? "ok" : "FAILED (different game build?)", g_drivePuppet);
		if (g_processEvent) CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	}
	return TRUE;
}
