// ArkWeb guest: Marvel's Spider-Man Remastered side (winmm.dll proxy).
//
// Phase 1:
//  - publishes the hero's world transform (GuestState) about every 4 ms,
//  - plays a virtual XInput pad from the link (PadState) as controller 0,
//  - fakes window focus while the link is up (or ForceFocus=1), so the game keeps running in the
//    background: GetForegroundWindow/GetFocus lie, deactivation messages are swallowed, and
//    ClipCursor/SetCursorPos are ignored while the game isn't really in front.
#include "../common/fwd_winmm.h"
#include "../common/coords.h"
#include "../common/link.h"
#include "../common/util.h"
#include "gotham.h"
#include "hints.h"
#include "capture.h"
#include "zip.h"

#include <Xinput.h>
#include <atomic>
#include <cmath>
#include <vector>

using namespace arkweb;

namespace
{
	// ---- Spider-Man.exe 4.0630.0.0 (ArkWeb/recon/PHASE0b.md) ----------------------------------------
	constexpr uintptr_t kHeroLocalVtable = 0x38a93c8;  // Hero::HeroLocal
	constexpr int       kHeroLocalSlots = 24;
	// [exe+7b11770] = nx app; app +0xa0 = nx::NxGameWindowImpl. Its IsActive() (vtable +0x68) is
	// flag(+0x18) && flag(+0x19) && !flag(+0x1a) && !flag(+0x1b); the window procedure only reads raw
	// input while it is true. +0x19 drops when the window loses focus.
	constexpr uintptr_t kNxApp = 0x7b11770;
	constexpr int       kNxAppWindow = 0xa0, kWindowActiveA = 0x18, kWindowActiveB = 0x19;
	// HeroLocal +0x8 -> actor record; record +0x0 -> transform: rows +0x00/+0x10/+0x20, position +0x30,
	// bounds half extents +0x50.

	HMODULE g_self = nullptr;
	Link    g_link;
	int     g_forceFocus = 0;

	void* volatile    g_heroLocal = nullptr;  // captured from any HeroLocal virtual call
	std::atomic<bool> g_fakeFocus{ false };
	HWND              g_window = nullptr;
	WNDPROC           g_origWndProc = nullptr;

	// ---- hero transform ------------------------------------------------------------------------------
	struct HeroXf
	{
		float rows[3][4];
		float pos[4];
		float boundsCenter[4];
		float halfExtents[4];
	};

	bool ReadHero(HeroXf& a_out)
	{
		uintptr_t hero = reinterpret_cast<uintptr_t>(g_heroLocal);
		uintptr_t record = ReadOr<uintptr_t>(hero + 0x8, 0);
		uintptr_t xf = ReadOr<uintptr_t>(record, 0);
		if (!hero || !record || !xf) return false;
		HeroXf a, b;
		for (int i = 0; i < 4; ++i) {  // read until two reads agree: the game writes it on its own thread
			if (!Read(xf, a) || !Read(xf, b)) return false;
			if (memcmp(&a, &b, sizeof(a)) == 0) break;
		}
		for (float f : a.pos) {
			if (!std::isfinite(f) || std::fabs(f) > 1.0e5f) return false;
		}
		// all zero: the game is (re)creating the hero (a respawn, a load) - not a position
		if (std::fabs(a.pos[0]) + std::fabs(a.pos[1]) + std::fabs(a.pos[2]) < 1e-3f) return false;
		a_out = a;
		return true;
	}

	// ---- virtual pad -----------------------------------------------------------------------------------
	using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
	using XInputGetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
	XInputGetStateFn g_realGetState = nullptr;
	XInputGetStateFn g_realGetStateEx = nullptr;
	XInputGetCapsFn  g_realGetCaps = nullptr;

	std::atomic<int> g_steerRX{ 0 };  // camera steering on the served right stick (controller mode)

	bool VirtualPad(XINPUT_STATE* a_state)
	{
		if (!g_link.Valid()) return false;
		proto::PadState p;
		if (!SeqRead(g_link.Pad(), p) || !(p.flags & proto::kPadActive)) return false;
		a_state->dwPacketNumber = p.packet;
		a_state->Gamepad.wButtons = p.buttons;
		a_state->Gamepad.bLeftTrigger = p.leftTrigger;
		a_state->Gamepad.bRightTrigger = p.rightTrigger;
		a_state->Gamepad.sThumbLX = p.thumbLX;
		a_state->Gamepad.sThumbLY = p.thumbLY;
		a_state->Gamepad.sThumbRX = (p.flags & proto::kPadCamera) ? static_cast<SHORT>(g_steerRX.load(std::memory_order_relaxed)) : p.thumbRX;
		a_state->Gamepad.sThumbRY = p.thumbRY;
		zip::OnPad(p.leftTrigger, p.rightTrigger, p.buttons);  // L2 + R2: zip to the targeted grapple ledge; A: launch off it (zip.h)
		return true;
	}

	std::atomic<uint64_t> g_padPolls{ 0 }, g_padServed{ 0 };

	DWORD WINAPI GetStateDetour(DWORD a_user, XINPUT_STATE* a_state)
	{
		g_padPolls.fetch_add(1, std::memory_order_relaxed);
		if (a_user == 0 && a_state && VirtualPad(a_state)) {
			g_padServed.fetch_add(1, std::memory_order_relaxed);
			return ERROR_SUCCESS;
		}
		return g_realGetState ? g_realGetState(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
	}

	DWORD WINAPI GetStateExDetour(DWORD a_user, XINPUT_STATE* a_state)
	{
		g_padPolls.fetch_add(1, std::memory_order_relaxed);
		if (a_user == 0 && a_state && VirtualPad(a_state)) {
			g_padServed.fetch_add(1, std::memory_order_relaxed);
			return ERROR_SUCCESS;
		}
		return g_realGetStateEx ? g_realGetStateEx(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
	}

	DWORD WINAPI GetCapsDetour(DWORD a_user, DWORD a_flags, XINPUT_CAPABILITIES* a_caps)
	{
		XINPUT_STATE s;
		if (a_user == 0 && a_caps && VirtualPad(&s)) {
			memset(a_caps, 0, sizeof(*a_caps));
			a_caps->Type = XINPUT_DEVTYPE_GAMEPAD;
			a_caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
			a_caps->Gamepad.wButtons = 0xF3FF;
			a_caps->Gamepad.bLeftTrigger = a_caps->Gamepad.bRightTrigger = 0xFF;
			a_caps->Gamepad.sThumbLX = a_caps->Gamepad.sThumbLY = a_caps->Gamepad.sThumbRX = a_caps->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
			return ERROR_SUCCESS;
		}
		return g_realGetCaps ? g_realGetCaps(a_user, a_flags, a_caps) : ERROR_DEVICE_NOT_CONNECTED;
	}

	using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
	GetProcAddressFn g_realGetProcAddress = nullptr;

	FARPROC WINAPI GetProcAddressDetour(HMODULE a_mod, LPCSTR a_name)
	{
		FARPROC real = g_realGetProcAddress(a_mod, a_name);
		if (!real) return real;
		char mod[MAX_PATH] = "";
		GetModuleFileNameA(a_mod, mod, MAX_PATH);
		_strlwr_s(mod);
		// D3D12: the device's creation is where the capture hooks Direct3D (capture.h)
		if (reinterpret_cast<uintptr_t>(a_name) >= 0x10000 && !strcmp(a_name, "D3D12CreateDevice")) {
			auto fn = reinterpret_cast<capture::CreateDeviceFn>(real);
			if (capture::g_realCreateDevice && capture::g_realCreateDevice != fn) return real;
			capture::g_realCreateDevice = fn;
			Log("D3D12CreateDevice from %s -> capture hooks", mod);
			return reinterpret_cast<FARPROC>(&capture::CreateDeviceDetour);
		}
		if (!strstr(mod, "xinput")) return real;
		if (reinterpret_cast<uintptr_t>(a_name) == 100) {  // XInputGetStateEx (ordinal)
			g_realGetStateEx = reinterpret_cast<XInputGetStateFn>(real);
			Log("XInputGetStateEx (#100) from %s -> virtual pad", mod);
			return reinterpret_cast<FARPROC>(&GetStateExDetour);
		}
		if (reinterpret_cast<uintptr_t>(a_name) < 0x10000) return real;
		if (!strcmp(a_name, "XInputGetState")) {
			g_realGetState = reinterpret_cast<XInputGetStateFn>(real);
			Log("XInputGetState from %s -> virtual pad", mod);
			return reinterpret_cast<FARPROC>(&GetStateDetour);
		}
		if (!strcmp(a_name, "XInputGetCapabilities")) {
			g_realGetCaps = reinterpret_cast<XInputGetCapsFn>(real);
			return reinterpret_cast<FARPROC>(&GetCapsDetour);
		}
		return real;
	}

	// ---- settings overrides ----------------------------------------------------------------------------
	// Spider-Man reads its settings from HKCU\Software\Insomniac Games\...\Input. With
	// EnableWindowsGamingInput=1 it reads controllers through Windows.Gaming.Input and switches XInput
	// off, so the virtual pad (XInput) is never polled. While VirtualPad=1 (arkweb.ini, default) the
	// game is told the value is 0 for this run; the user's registry is never written.
	int g_virtualPadSetting = 1;

	bool OverrideDword(const wchar_t* a_name, DWORD& a_value)
	{
		if (g_virtualPadSetting && a_name && !_wcsicmp(a_name, L"EnableWindowsGamingInput")) {
			a_value = 0;
			return true;
		}
		return false;
	}

	LSTATUS FillDword(DWORD a_value, LPDWORD a_type, LPBYTE a_data, LPDWORD a_size)
	{
		if (a_type) *a_type = REG_DWORD;
		if (a_data && a_size && *a_size < 4) {
			*a_size = 4;
			return ERROR_MORE_DATA;
		}
		if (a_data) memcpy(a_data, &a_value, 4);
		if (a_size) *a_size = 4;
		return ERROR_SUCCESS;
	}

	using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	using RegQueryValueExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	using RegGetValueAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
	RegQueryValueExWFn g_realRegQueryW = nullptr;
	RegQueryValueExAFn g_realRegQueryA = nullptr;
	RegGetValueAFn     g_realRegGetA = nullptr;

	std::wstring Widen(const char* a_s)
	{
		std::wstring w;
		for (; a_s && *a_s; ++a_s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*a_s)));
		return w;
	}

	LSTATUS WINAPI RegQueryWDetour(HKEY a_key, LPCWSTR a_name, LPDWORD a_res, LPDWORD a_type, LPBYTE a_data, LPDWORD a_size)
	{
		DWORD v;
		if (OverrideDword(a_name, v)) {
			Log("setting %ls -> %lu (ArkWeb VirtualPad)", a_name, v);
			return FillDword(v, a_type, a_data, a_size);
		}
		return g_realRegQueryW(a_key, a_name, a_res, a_type, a_data, a_size);
	}

	LSTATUS WINAPI RegQueryADetour(HKEY a_key, LPCSTR a_name, LPDWORD a_res, LPDWORD a_type, LPBYTE a_data, LPDWORD a_size)
	{
		DWORD v;
		if (OverrideDword(Widen(a_name).c_str(), v)) {
			Log("setting %s -> %lu (ArkWeb VirtualPad)", a_name, v);
			return FillDword(v, a_type, a_data, a_size);
		}
		return g_realRegQueryA(a_key, a_name, a_res, a_type, a_data, a_size);
	}

	LSTATUS WINAPI RegGetADetour(HKEY a_key, LPCSTR a_sub, LPCSTR a_name, DWORD a_flags, LPDWORD a_type, PVOID a_data, LPDWORD a_size)
	{
		DWORD v;
		if (OverrideDword(Widen(a_name).c_str(), v)) {
			Log("setting %s -> %lu (ArkWeb VirtualPad)", a_name, v);
			return FillDword(v, a_type, static_cast<LPBYTE>(a_data), a_size);
		}
		return g_realRegGetA(a_key, a_sub, a_name, a_flags, a_type, a_data, a_size);
	}

	// ---- keyboard injection (raw input) ------------------------------------------------------------------
	// Spider-Man only polls XInput slots where it found a real Xbox device, so the virtual pad drives
	// the game through its KEYBOARD bindings instead: pad changes become WM_INPUT messages posted to
	// the game window, whose GetRawInputData calls are answered here for our own handles.
	struct KeyBind
	{
		const wchar_t* setting;  // registry value (Insomniac Games\...\Input) holding the scan code
		uint16_t       fallback; // default scan code
	};
	enum Key
	{
		kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyJump, kKeySwing, kKeyDodge, kKeyCount
	};
	const KeyBind kBinds[kKeyCount] = {
		{ L"Mkb_LeftStick_Up_1", 0x11 }, { L"Mkb_LeftStick_Down_1", 0x1f }, { L"Mkb_LeftStick_Left_1", 0x1e },
		{ L"Mkb_LeftStick_Right_1", 0x20 }, { L"Button_A_1", 0x39 }, { L"Right_Trigger_1", 0x2a }, { L"Button_B_1", 0x1d },
	};
	uint16_t g_scan[kKeyCount] = {};
	bool     g_keyDown[kKeyCount] = {};
	HANDLE   g_keyboardDevice = nullptr;

	constexpr uintptr_t kFakeTag = 0x7A5E000000000000ull, kFakeMask = 0xFFFF000000000000ull;
	struct FakeKey
	{
		uint16_t scan;
		bool     down;
		bool     mouse;   // a relative mouse move (dx, dy) instead of a key
		int32_t  dx, dy;
	};
	HANDLE g_mouseDevice = nullptr;
	FakeKey               g_fake[256];
	std::atomic<uint32_t> g_fakeNext{ 0 };
	std::atomic<uint64_t> g_fakeServed{ 0 };

	using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
	GetRawInputDataFn g_realGetRawInputData = nullptr;

	UINT WINAPI GetRawInputDataDetour(HRAWINPUT a_h, UINT a_cmd, LPVOID a_data, PUINT a_size, UINT a_hdr)
	{
		uintptr_t h = reinterpret_cast<uintptr_t>(a_h);
		if ((h & kFakeMask) != kFakeTag) return g_realGetRawInputData(a_h, a_cmd, a_data, a_size, a_hdr);
		const FakeKey& k = g_fake[h & 0xFF];
		RAWINPUT ri{};
		if (k.mouse) {
			ri.header.dwType = RIM_TYPEMOUSE;
			ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
			ri.header.hDevice = g_mouseDevice;
			ri.header.wParam = RIM_INPUT;
			ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
			ri.data.mouse.lLastX = k.dx;
			ri.data.mouse.lLastY = k.dy;
			UINT need = a_cmd == RID_HEADER ? sizeof(RAWINPUTHEADER) : ri.header.dwSize;
			if (!a_data) {
				*a_size = need;
				return 0;
			}
			if (*a_size < need) return static_cast<UINT>(-1);
			memcpy(a_data, &ri, need);
			g_fakeServed.fetch_add(1, std::memory_order_relaxed);
			return need;
		}
		ri.header.dwType = RIM_TYPEKEYBOARD;
		ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
		ri.header.hDevice = g_keyboardDevice;
		ri.header.wParam = RIM_INPUT;
		ri.data.keyboard.MakeCode = k.scan & 0xFF;
		ri.data.keyboard.Flags = static_cast<USHORT>((k.down ? RI_KEY_MAKE : RI_KEY_BREAK) | ((k.scan & 0xE000) ? RI_KEY_E0 : 0));
		ri.data.keyboard.VKey = static_cast<USHORT>(MapVirtualKeyW(k.scan & 0xFF, MAPVK_VSC_TO_VK_EX));
		ri.data.keyboard.Message = k.down ? WM_KEYDOWN : WM_KEYUP;
		UINT need = a_cmd == RID_HEADER ? sizeof(RAWINPUTHEADER) : ri.header.dwSize;
		if (!a_data) {
			*a_size = need;
			return 0;
		}
		if (*a_size < need) return static_cast<UINT>(-1);
		memcpy(a_data, &ri, need);
		g_fakeServed.fetch_add(1, std::memory_order_relaxed);
		return need;
	}

	void SendKey(int a_key, bool a_down)
	{
		if (g_keyDown[a_key] == a_down || !g_window) return;
		g_keyDown[a_key] = a_down;
		uint32_t n = g_fakeNext.fetch_add(1) & 0xFF;
		g_fake[n] = { g_scan[a_key], a_down, false, 0, 0 };
		PostMessageW(g_window, WM_INPUT, RIM_INPUT, static_cast<LPARAM>(kFakeTag | (g_fakeNext.load() & 0xFF00) | n));
	}

	void SendMouse(int32_t a_dx, int32_t a_dy)
	{
		if ((!a_dx && !a_dy) || !g_window) return;
		uint32_t n = g_fakeNext.fetch_add(1) & 0xFF;
		g_fake[n] = { 0, false, true, a_dx, a_dy };
		PostMessageW(g_window, WM_INPUT, RIM_INPUT, static_cast<LPARAM>(kFakeTag | (g_fakeNext.load() & 0xFF00) | n));
	}

	void InitKeyboardInjection()
	{
		for (int i = 0; i < kKeyCount; ++i) {
			DWORD v = 0, sz = sizeof(v);
			g_scan[i] = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Insomniac Games\\Marvel's Spider-Man Remastered\\Input", kBinds[i].setting,
							RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v && v < 0x100 ? static_cast<uint16_t>(v) : kBinds[i].fallback;
		}
		UINT n = 0;
		GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
		std::vector<RAWINPUTDEVICELIST> list(n);
		if (n && GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1)) {
			for (auto& d : list) {
				if (d.dwType == RIM_TYPEKEYBOARD && !g_keyboardDevice) g_keyboardDevice = d.hDevice;
				if (d.dwType == RIM_TYPEMOUSE && !g_mouseDevice) g_mouseDevice = d.hDevice;
			}
		}
		Log("keyboard injection: W %02x S %02x A %02x D %02x jump %02x swing %02x dodge %02x, keyboard %p, mouse %p", g_scan[0], g_scan[1],
			g_scan[2], g_scan[3], g_scan[4], g_scan[5], g_scan[6], g_keyboardDevice, g_mouseDevice);
	}

	// ---- camera steering: Spider-Man's movement and swings are relative to ITS camera, so the camera is
	// turned (injected mouse moves) to the yaw the host camera has. Hero::HeroCameraManager +0x744 holds
	// the camera matrix (rows side / up / forward / position, 4-byte aligned). Mouse counts per radian are
	// calibrated once (a known move while nothing else turns the camera), then a P-controller steers.
	constexpr uintptr_t kHeroCameraManagerVtable = 0x38b1dd0;
	constexpr int       kHeroCameraManagerSlots = 38;
	constexpr int       kCameraMatrix = 0x744;
	void* volatile      g_heroCamera = nullptr;
	double              g_countsPerRad = 0.0;  // signed; 0 = not calibrated
	int                 g_calibStage = 0;      // 0 idle, 1 probe sent
	double              g_calibYaw = 0.0;
	ULONGLONG           g_calibMs = 0, g_lastSteerMs = 0;
	int                 g_calibTries = 0;

	double WrapPi(double a_r)
	{
		while (a_r > coords::kPi) a_r -= 2 * coords::kPi;
		while (a_r < -coords::kPi) a_r += 2 * coords::kPi;
		return a_r;
	}

	bool CameraYaw(double& a_yaw)
	{
		float f[3];
		auto  cam = reinterpret_cast<uintptr_t>(g_heroCamera);
		if (!cam || !SafeRead(f, reinterpret_cast<const void*>(cam + kCameraMatrix + 0x20), sizeof(f))) return false;
		if (!std::isfinite(f[0]) || !std::isfinite(f[2]) || std::fabs(f[0]) + std::fabs(f[2]) < 0.05f) return false;
		a_yaw = std::atan2(f[0], f[2]);
		return true;
	}

	// Calibration: watch the camera's own drift for 0.3 s, then push (full stick / a steady mouse
	// stream) for 0.5 s and take the drift-corrected turn: its sign, and for the mouse its counts per
	// radian. A guard flips the sign if the camera keeps turning against a hard push, so even a bad
	// calibration corrects itself. "cam off|on|recal" from sm_cmd.txt.
	std::atomic<bool> g_steerEnabled{ true };
	std::atomic<bool> g_steerRecal{ false };
	int               g_sign = 0;  // +1: positive input raises the yaw; 0 = not calibrated
	ULONGLONG         g_stageMs = 0, g_guardMs = 0;
	double            g_stageYaw = 0.0, g_driftPerMs = 0.0, g_guardYaw = 0.0;
	int               g_probeCounts = 0, g_guardWant = 0, g_guardTicks = 0;

	void SteerCamera(const proto::PadState& a_p)
	{
		ULONGLONG now = GetTickCount64();
		double    yaw;
		bool      stick = (a_p.flags & proto::kPadAnalog) != 0;
		if (g_steerRecal.exchange(false)) g_sign = 0, g_calibStage = 0, g_steerRX = 0;
		if (!(a_p.flags & proto::kPadCamera) || !g_steerEnabled || now - g_lastSteerMs < 16 || !CameraYaw(yaw)) {
			if (!g_steerEnabled) g_steerRX = 0;
			return;
		}
		g_lastSteerMs = now;
		if (g_sign == 0) {
			if (g_calibStage == 0) {  // watch the drift
				g_steerRX = 0;
				g_stageYaw = yaw, g_stageMs = now, g_calibStage = 1;
			} else if (g_calibStage == 1 && now - g_stageMs >= 300) {  // push
				g_driftPerMs = WrapPi(yaw - g_stageYaw) / static_cast<double>(now - g_stageMs);
				g_stageYaw = yaw, g_stageMs = now, g_calibStage = 2, g_probeCounts = 0;
			} else if (g_calibStage == 2 && now - g_stageMs < 500) {
				if (stick) g_steerRX = 32767;
				else SendMouse(150, 0), g_probeCounts += 150;
			} else if (g_calibStage == 2) {  // measure
				g_steerRX = 0;
				double moved = WrapPi(yaw - g_stageYaw) - g_driftPerMs * static_cast<double>(now - g_stageMs);
				g_calibStage = 0;
				if (std::fabs(moved) > 0.2) {
					g_sign = moved > 0 ? 1 : -1;
					if (!stick) g_countsPerRad = g_probeCounts / std::fabs(moved);
					Log("camera: calibrated (%s): %+.2f rad net in 0.5 s (drift %.4f rad/s)%s", stick ? "stick" : "mouse", moved, g_driftPerMs * 1000.0,
						stick ? "" : (" - " + std::to_string(static_cast<int>(g_countsPerRad)) + " counts/rad").c_str());
				} else if (++g_calibTries % 5 == 0) {
					Log("camera: probes turned only %+.3f rad net, still trying", moved);
				}
			}
			return;
		}
		double err = WrapPi(static_cast<double>(a_p.camYaw) - yaw);
		bool   hard;
		if (stick) {
			// past the stick deadzone; gentle near the target (the camera has momentum and overshoots)
			double mag = std::fabs(err) < 0.03 ? 0.0 : std::min(32767.0, 9000.0 + std::fabs(err) * 30000.0);
			g_steerRX = static_cast<int>(g_sign * (err > 0 ? mag : -mag));
			hard = mag >= 32767.0;
		} else {
			double dx = std::fabs(err) < 0.01 ? 0.0 : std::min(800.0, std::fabs(err) * g_countsPerRad * 0.35);
			SendMouse(static_cast<int32_t>(std::lround(g_sign * (err > 0 ? dx : -dx))), 0);
			hard = dx >= 800.0;
		}
		// Wrong-sign guard: pushing hard the same way for a whole second while the camera turned the
		// other way by more than 0.5 rad. (A growing error is no proof: the target moves, and the
		// camera's momentum overshoots - that guard flipped a correct sign back and forth.)
		int want = err > 0 ? 1 : -1;
		if (!hard || (g_guardTicks && want != g_guardWant)) {
			g_guardTicks = 0;
		} else if (!g_guardTicks++) {
			g_guardWant = want, g_guardYaw = yaw, g_guardMs = now;
		} else if (now - g_guardMs >= 1000) {
			double moved = WrapPi(yaw - g_guardYaw);
			if (moved * want < -0.5) {
				g_sign = -g_sign;
				Log("camera: pushed toward %+d for 1 s but turned %+.2f rad - sign flipped", want, moved);
			}
			g_guardTicks = 0;
		}
	}

	// Pad -> keys, from the worker every 4 ms.
	void PumpPadToKeys()
	{
		proto::PadState p{};
		bool active = g_link.Valid() && SeqRead(g_link.Pad(), p) && (p.flags & proto::kPadActive);
		constexpr int kDeadzone = 16000;
		bool want[kKeyCount] = {};
		if (active && (p.flags & proto::kPadAnalog)) {
			SteerCamera(p);  // the pad itself is served through XInput
		} else if (active) {
			want[kKeyUp] = p.thumbLY > kDeadzone;
			want[kKeyDown] = p.thumbLY < -kDeadzone;
			want[kKeyRight] = p.thumbLX > kDeadzone;
			want[kKeyLeft] = p.thumbLX < -kDeadzone;
			want[kKeyJump] = (p.buttons & XINPUT_GAMEPAD_A) != 0;
			want[kKeyDodge] = (p.buttons & XINPUT_GAMEPAD_B) != 0;
			want[kKeySwing] = p.rightTrigger > 128;
			SteerCamera(p);
		}
		for (int i = 0; i < kKeyCount; ++i) SendKey(i, want[i]);
	}

	// ---- fake focus ------------------------------------------------------------------------------------
	using HwndFn = HWND(WINAPI*)();
	using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
	using SetCursorPosFn = BOOL(WINAPI*)(int, int);
	HwndFn         g_realGetForeground = nullptr;
	HwndFn         g_realGetFocus = nullptr;
	ClipCursorFn   g_realClipCursor = nullptr;
	SetCursorPosFn g_realSetCursorPos = nullptr;

	bool ReallyInFront() { return g_window && g_realGetForeground() == g_window; }

	HWND WINAPI GetForegroundDetour()
	{
		if (g_fakeFocus && g_window) return g_window;
		return g_realGetForeground();
	}

	HWND WINAPI GetFocusDetour()
	{
		if (g_fakeFocus && g_window) return g_window;
		return g_realGetFocus();
	}

	BOOL WINAPI ClipCursorDetour(const RECT* a_rect)
	{
		if (g_fakeFocus && !ReallyInFront()) return g_realClipCursor(nullptr);  // never trap the mouse in a background game
		return g_realClipCursor(a_rect);
	}

	BOOL WINAPI SetCursorPosDetour(int a_x, int a_y)
	{
		if (g_fakeFocus && !ReallyInFront()) return TRUE;
		return g_realSetCursorPos(a_x, a_y);
	}

	LRESULT CALLBACK WndProcDetour(HWND a_wnd, UINT a_msg, WPARAM a_wp, LPARAM a_lp)
	{
		if (g_fakeFocus) {
			if ((a_msg == WM_ACTIVATEAPP && !a_wp) || (a_msg == WM_ACTIVATE && LOWORD(a_wp) == WA_INACTIVE) || a_msg == WM_KILLFOCUS) {
				return 0;
			}
		}
		return CallWindowProcW(g_origWndProc, a_wnd, a_msg, a_wp, a_lp);
	}

	// The game window is class "GameNxApp" (the launcher/splash windows before it are destroyed).
	BOOL CALLBACK FindGameWindow(HWND a_wnd, LPARAM)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(a_wnd, &pid);
		if (pid != GetCurrentProcessId()) return TRUE;
		wchar_t cls[64] = L"";
		GetClassNameW(a_wnd, cls, 64);
		if (wcscmp(cls, L"GameNxApp") != 0) return TRUE;
		g_window = a_wnd;
		return FALSE;
	}

	// The game only reads raw input while its own "app active" flag is set, which follows
	// WM_ACTIVATEAPP. When focus faking starts (often after the user already switched away), tell
	// the window it is active again.
	// While focus is faked, keep the game window object's active flags set.
	void HoldWindowActive()
	{
		uintptr_t app = ReadOr<uintptr_t>(g_exe + kNxApp, 0);
		uintptr_t win = ReadOr<uintptr_t>(app + kNxAppWindow, 0);
		uint8_t   a = 0, b = 0;
		if (!win || !Read(win + kWindowActiveA, a) || !Read(win + kWindowActiveB, b)) return;
		if (!a) *reinterpret_cast<volatile uint8_t*>(win + kWindowActiveA) = 1;
		if (!b) *reinterpret_cast<volatile uint8_t*>(win + kWindowActiveB) = 1;
	}

	void PostActivation()
	{
		if (!g_window) return;
		PostMessageW(g_window, WM_ACTIVATEAPP, TRUE, 0);
		PostMessageW(g_window, WM_ACTIVATE, WA_ACTIVE, 0);
		PostMessageW(g_window, WM_SETFOCUS, 0, 0);
	}

	// ---- game frame hook + query hook (Phase 2) ---------------------------------------------------------
	constexpr uintptr_t kCastRay = 0x2e67010;      // hknpWorld::castRay
	constexpr uint8_t   kCastRaySig[15] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };

	using Fn6 = void* (*)(void*, void*, void*, void*, void*, void*);
	Fn6               g_origCastRay = nullptr;
	std::atomic<bool> g_filterPatchClaimed{ false };

	// hknpWorld::castShape / getClosestPoints share castRay's prologue (PHASE0b.md). They are hooked only
	// to tag the filter calls they make (gotham query stats).
	constexpr uintptr_t kCastShape = 0x2e670f0, kClosestPoints = 0x2e671f0;
	Fn6 g_origCastShape = nullptr, g_origClosest = nullptr;

	// Runs a_orig with the thread's query-entry tag set, restoring the outer tag after (queries nest).
	inline void* TaggedQuery(Fn6 a_orig, int a_entry, void* a_world, void* a_query, void* a_c, void* a_d, void* a_e, void* a_f)
	{
		int outer = gotham::g_entry;
		gotham::g_entry = a_entry;
		gotham::g_entryCalls[a_entry].fetch_add(1, std::memory_order_relaxed);
		void* r = a_orig(a_world, a_query, a_c, a_d, a_e, a_f);
		gotham::g_entry = outer;
		return r;
	}

	// castRay(world, query, collector); castShape(world, query, shape transform, collector, start collector)
	void* CastRayDetour(void* a_world, void* a_query, void* a_c, void* a_d, void* a_e, void* a_f)
	{
		if (!gotham::g_patchedVtable && !g_filterPatchClaimed.exchange(true)) gotham::PatchFilterFrom(a_query);
		void* r = TaggedQuery(g_origCastRay, gotham::kEntryRay, a_world, a_query, a_c, a_d, a_e, a_f);
		gotham::SampleHits(a_c);
		return r;
	}

	void* CastShapeDetour(void* a_world, void* a_query, void* a_c, void* a_d, void* a_e, void* a_f)
	{
		void* r = TaggedQuery(g_origCastShape, gotham::kEntryShape, a_world, a_query, a_c, a_d, a_e, a_f);
		gotham::SampleHits(a_d);
		return r;
	}

	void* ClosestDetour(void* a_world, void* a_query, void* a_c, void* a_d, void* a_e, void* a_f)
	{
		return TaggedQuery(g_origClosest, gotham::kEntryClosest, a_world, a_query, a_c, a_d, a_e, a_f);
	}

	// ---- teleport -------------------------------------------------------------------------------------
	// The hero's position is kept in more than one place (the actor transform and whatever moves it);
	// moving only one gets it written back the next frame. So every float triple that equals the hero's
	// position in the hero objects - and in the objects they point to - is moved together, a few times
	// over a few milliseconds to beat the game's own writes, and the result is checked.
	struct TeleportResult
	{
		bool   ok;
		int    copies;
		double error;  // meters from the target afterwards
	};
	ULONGLONG g_lastTeleportMs = 0;

	void ScanForTriple(uintptr_t a_base, size_t a_bytes, const float a_pos[3], std::vector<uintptr_t>& a_out, std::vector<uintptr_t>* a_ptrs)
	{
		std::vector<uint8_t> buf(a_bytes);
		size_t               got = 0;
		while (got < a_bytes) {  // page by page, up to the first unreadable one
			size_t chunk = std::min<size_t>(0x1000 - ((a_base + got) & 0xFFF), a_bytes - got);
			if (!SafeRead(buf.data() + got, reinterpret_cast<const void*>(a_base + got), chunk)) break;
			got += chunk;
		}
		for (size_t o = 0; o + 12 <= got; o += 4) {
			float v[3];
			memcpy(v, buf.data() + o, 12);
			if (std::fabs(v[0] - a_pos[0]) < 0.01f && std::fabs(v[1] - a_pos[1]) < 0.01f && std::fabs(v[2] - a_pos[2]) < 0.01f) a_out.push_back(a_base + o);
		}
		if (!a_ptrs) return;
		for (size_t o = 0; o + 8 <= got; o += 8) {
			uintptr_t p;
			memcpy(&p, buf.data() + o, 8);
			if (p > 0x10000 && p < 0x7FFFFFFF0000ull && !(p & 7) && !InModule(p, g_exe)) a_ptrs->push_back(p);
		}
	}

	std::vector<uintptr_t> FindPositionCopies(const float a_pos[3])
	{
		std::vector<uintptr_t> copies, ptrs;
		uintptr_t              hero = reinterpret_cast<uintptr_t>(g_heroLocal);
		uintptr_t              record = ReadOr<uintptr_t>(hero + 0x8, 0);
		ScanForTriple(hero, 0x2000, a_pos, copies, &ptrs);
		if (record) ScanForTriple(record, 0x800, a_pos, copies, &ptrs);
		std::sort(ptrs.begin(), ptrs.end());
		ptrs.erase(std::unique(ptrs.begin(), ptrs.end()), ptrs.end());
		if (ptrs.size() > 400) ptrs.resize(400);
		for (uintptr_t p : ptrs) ScanForTriple(p, 0x600, a_pos, copies, nullptr);
		std::sort(copies.begin(), copies.end());
		copies.erase(std::unique(copies.begin(), copies.end()), copies.end());
		return copies;
	}

	TeleportResult Teleport(const double a_target[3])
	{
		HeroXf xf;
		if (!ReadHero(xf)) return { false, 0, 0.0 };
		const float from[3] = { xf.pos[0], xf.pos[1], xf.pos[2] };
		const float delta[3] = { static_cast<float>(a_target[0] - from[0]), static_cast<float>(a_target[1] - from[1]),
			static_cast<float>(a_target[2] - from[2]) };
		std::vector<uintptr_t> copies = FindPositionCopies(from);
		// The hero's position is kept in a couple of places, not thousands: many matches mean the value is
		// a common one (zero, a default) and writing them all would corrupt the game (it did, 2026-10-01).
		if (copies.empty() || copies.size() > 12) {
			Log("teleport refused: %zu copies of (%.2f %.2f %.2f) found", copies.size(), from[0], from[1], from[2]);
			return { false, static_cast<int>(copies.size()), 1e9 };
		}
		for (int pass = 0; pass < 10; ++pass) {
			for (uintptr_t a : copies) {
				float v[3];
				if (!SafeRead(v, reinterpret_cast<const void*>(a), 12)) continue;
				// still at the old spot (or the game wrote it back): move it
				if (std::fabs(v[0] - from[0]) < 0.5f && std::fabs(v[1] - from[1]) < 0.5f && std::fabs(v[2] - from[2]) < 0.5f) {
					for (int i = 0; i < 3; ++i) v[i] += delta[i];
					SafeWrite(reinterpret_cast<void*>(a), v, 12);
				}
			}
			if (pass == 0) {  // the camera follows on its own; moving it too saves a 1 km swoop
				auto cam = reinterpret_cast<uintptr_t>(g_heroCamera);
				float c[3];
				if (cam && SafeRead(c, reinterpret_cast<const void*>(cam + kCameraMatrix + 0x30), 12)) {
					for (int i = 0; i < 3; ++i) c[i] += delta[i];
					SafeWrite(reinterpret_cast<void*>(cam + kCameraMatrix + 0x30), c, 12);
				}
			}
			Sleep(3);
		}
		g_lastTeleportMs = GetTickCount64();
		Sleep(250);
		double err = 1e9;
		if (ReadHero(xf)) err = std::sqrt(std::pow(xf.pos[0] - a_target[0], 2) + std::pow(xf.pos[1] - a_target[1], 2) + std::pow(xf.pos[2] - a_target[2], 2));
		double jump = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
		bool   ok = err < 8.0 || err < 0.2 * jump;  // he keeps his speed: after 250 ms he may have run or fallen a bit
		Log("teleport (%.1f %.1f %.1f) -> (%.1f %.1f %.1f): %zu position copies moved, now %.1f m from the target - %s", from[0], from[1], from[2],
			a_target[0], a_target[1], a_target[2], copies.size(), err, ok ? "held" : "DID NOT HOLD");
		if (!ok) {
			for (size_t i = 0; i < copies.size() && i < 24; ++i) {
				float v[3] = {};
				SafeRead(v, reinterpret_cast<const void*>(copies[i]), 12);
				Log("   copy %p now (%.2f %.2f %.2f)", reinterpret_cast<void*>(copies[i]), v[0], v[1], v[2]);
			}
		}
		return { ok, static_cast<int>(copies.size()), err };
	}

	// Requests for the worker (it may sleep; the pump thread may not).
	std::atomic<int> g_tpRequest{ 0 };  // 1 jump onto Gotham, 2 manual teleport
	double           g_tpTarget[3] = {};
	std::atomic<bool> g_skyActive{ false }, g_jumpFailed{ false };
	std::atomic<int>  g_rescues{ 0 };

	void RunCommand(char* a_line);

	// Developer commands (logs\sm_cmd.txt), run on the main thread from the frame hook.
	void PollCommands()
	{
		std::wstring file = g_logDir + L"\\sm_cmd.txt";
		if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return;
		FILE* f = _wfopen(file.c_str(), L"r");
		if (!f) return;
		std::vector<std::string> lines;
		char buf[512];
		while (fgets(buf, sizeof(buf), f)) lines.emplace_back(buf);
		fclose(f);
		DeleteFileW(file.c_str());
		for (auto& l : lines) {
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
			if (l.empty() || l[0] == '#') continue;
			Log("> %s", l.c_str());
			std::vector<char> w(l.begin(), l.end());
			w.push_back(0);
			RunCommand(w.data());
		}
		Log("< done");
	}

	// Gotham work runs at the start of hknpWorld's pre-collide phase (exe+2e54300, called once per
	// physics step from BuildCollideTasks): the previous step is finished, nothing collides yet,
	// and Havok itself commits pending body additions in this very function. PeekMessageW and
	// exe+16244c0 turned out not to run during gameplay.
	constexpr uintptr_t kPreCollide = 0x2e54300;
	constexpr uint8_t   kPreCollideSig[16] = { 0x41, 0x56, 0x48, 0x83, 0xec, 0x50, 0x48, 0x89, 0x5c, 0x24, 0x60, 0x48, 0x89, 0x6c, 0x24, 0x68 };
	using PreCollideFn = void* (*)(void*, void*, void*, void*);
	PreCollideFn g_origPreCollide = nullptr;
	ULONGLONG    g_lastPollMs = 0;
	DWORD        g_pumpThread = 0;
	uint64_t     g_steps = 0;

	// The streamer's commands and tiles arrive through the collision ring in order; at most one tile is
	// taken per physics step (parsing it copies a few hundred KB).
	Ring              g_ring;
	std::atomic<bool> g_ringReady{ false };
	uint32_t          g_ringTiles = 0;

	void DrainRing()
	{
		if (!g_ringReady.load(std::memory_order_acquire)) return;
		bool tookTile = false;
		for (int i = 0; i < 64 && !tookTile; ++i) {
			int n = g_ring.Drain(
				[&](uint32_t a_type, const uint8_t* a_p, uint32_t a_bytes) {
					if (a_type == proto::kRecCommand) {
						std::vector<char> line(a_p, a_p + a_bytes);
						line.push_back(0);
						if (strncmp(line.data(), "gotham stream unload", 20) != 0) Log("> %s", line.data());
						RunCommand(line.data());
					} else if (a_type == proto::kRecTile && a_bytes >= sizeof(proto::TileRecord)) {
						proto::TileRecord tr;
						memcpy(&tr, a_p, sizeof(tr));
						tr.key[sizeof(tr.key) - 1] = 0;
						if (sizeof(tr) + static_cast<uint64_t>(tr.awtBytes) + tr.awhBytes > a_bytes) return;
						const uint8_t* awt = a_p + sizeof(tr);
						if (gotham::StreamLoadMem(tr.key, awt, tr.awtBytes) && tr.awhBytes) {
							double offset[3];
							for (int k = 0; k < 3; ++k) offset[k] = gotham::g_place[k] - gotham::g_anchorGuest[k];
							hints::LoadMem(awt + tr.awtBytes, tr.awhBytes, offset, tr.key);
						}
						if (++g_ringTiles % 25 == 1) Log("ring: tile %s (%u so far, %u + %u bytes)", tr.key, g_ringTiles, tr.awtBytes, tr.awhBytes);
						tookTile = true;
					}
				},
				1);
			if (!n) break;
		}
	}

	void* PreCollideDetour(void* a_world, void* a_b, void* a_c, void* a_d)
	{
		++g_steps;
		if (a_world == hk::World()) {
			if (!g_pumpThread) {
				g_pumpThread = GetCurrentThreadId();
				Log("gotham: pumping in hknpWorld pre-collide on thread %lu", g_pumpThread);
			}
			ULONGLONG now = GetTickCount64();
			if (now - g_lastPollMs >= 250) {  // manual commands; the streamer uses the ring
				g_lastPollMs = now;
				PollCommands();
			}
			DrainRing();
			gotham::Pump();
			zip::PumpFallback();  // an L2 + R2 point launch the game's own call didn't take
			zip::PinStep();       // a perch stays where it is (zip.h)
		}
		return g_origPreCollide(a_world, a_b, a_c, a_d);
	}

	// gotham stream begin <x y z> | load <key> <file in logs\> | unload <key> | reset | status (tools/gotham_stream.py).
	// A load also takes <file>.awh (same name) as the tile's swing hints when it exists.
	void RunStreamCommand(const char* a_verb, char* a_a, char* a_b, char*& a_ctx)
	{
		if (!_stricmp(a_verb, "begin") && a_a && a_b) {
			char* z = strtok_s(nullptr, " \t", &a_ctx);
			char* sky = strtok_s(nullptr, " \t", &a_ctx);
			HeroXf xf;
			if (!z || !ReadHero(xf)) {
				Log("  usage: gotham stream begin <x y z Arkham feet UU> [sky m], with the hero in the world");
				return;
			}
			double origin[3] = { atof(a_a), atof(a_b), atof(z) };
			double hero[3] = { xf.pos[0], xf.pos[1], xf.pos[2] };
			g_skyActive = false;
			g_jumpFailed = false;
			gotham::StreamBegin(origin, hero, sky ? atof(sky) : 0.0);
		} else if (!_stricmp(a_verb, "jump")) {
			// onto Gotham's origin spot (Batman's feet), half a meter up
			g_tpTarget[0] = gotham::g_place[0], g_tpTarget[1] = gotham::g_place[1] + 0.5, g_tpTarget[2] = gotham::g_place[2];
			g_tpRequest = 1;
		} else if (!_stricmp(a_verb, "release") && a_a) {
			gotham::g_releaseShapes = !_stricmp(a_a, "on");
			Log("  shapes of unloaded tiles are %s", gotham::g_releaseShapes ? "released (heap freed)" : "kept");
		} else if (!_stricmp(a_verb, "buildms") && a_a) {
			gotham::g_buildMsPerStep = atof(a_a);
			Log("  a second tile part in the same physics step only under %.1f ms", gotham::g_buildMsPerStep.load());
		} else if (!_stricmp(a_verb, "heapguard") && a_a) {
			gotham::g_heapGuard = !_stricmp(a_a, "on");
			if (a_b) gotham::g_minFreeBytes = static_cast<uint64_t>(atof(a_b) * 1048576.0);
			Log("  heap guard %s (%.0f MB)", gotham::g_heapGuard ? "on" : "off", gotham::g_minFreeBytes / 1048576.0);
		} else if (!_stricmp(a_verb, "load") && a_a && a_b) {
			std::string  fn = a_b;
			std::wstring path = g_logDir + L"\\" + std::wstring(fn.begin(), fn.end());
			if (!gotham::StreamLoad(a_a, path)) return;
			std::wstring hpath = path.substr(0, path.find_last_of(L'.')) + L".awh";
			if (GetFileAttributesW(hpath.c_str()) != INVALID_FILE_ATTRIBUTES) {
				double offset[3];
				for (int i = 0; i < 3; ++i) offset[i] = gotham::g_place[i] - gotham::g_anchorGuest[i];
				hints::Load(hpath, offset, a_a, true);
			}
		} else if (!_stricmp(a_verb, "unload") && a_a) {
			gotham::StreamUnload(a_a);
			hints::Queue({ 2, a_a, {} });
		} else if (!_stricmp(a_verb, "reset")) {
			size_t n = gotham::g_stream.size();
			while (!gotham::g_stream.empty()) gotham::StreamUnload(gotham::g_stream.begin()->first);
			hints::Queue({ 3, {}, {} });
			g_skyActive = false;            // no rescues onto a Gotham that is gone
			gotham::g_anchorValid = false;  // Batman is released until the next begin
			gotham::g_streamOn = false;
			Log("  stream reset: %zu tiles unloaded", n);
		} else if (!_stricmp(a_verb, "status")) {
			gotham::HeapStats h = gotham::ReadHeap();
			Log("  stream: %zu tiles held, %zu waiting to build; %d loaded, %d unloaded, %d bodies destroyed, %d shapes released; %d hints; "
				"Havok heap %.0f MB used, %.0f MB free%s; builds refused %d",
				gotham::g_stream.size(), gotham::g_streamQueue.size(), gotham::g_streamLoaded.load(), gotham::g_streamUnloaded.load(),
				gotham::g_streamBodiesDestroyed.load(), gotham::g_shapesReleased.load(), hints::g_inserted.load(), h.used / 1048576.0,
				h.free / 1048576.0, h.valid ? "" : " (not readable)", gotham::g_buildsRefused.load());
		} else {
			Log("  usage: gotham stream begin x y z [sky] | jump | load key file | unload key | reset | release on|off | heapguard on|off [MB] | status");
		}
	}

	bool ReadCamera(float a_out[16]);

	void RunCommand(char* a_line)
	{
		char* ctx = nullptr;
		char* c = strtok_s(a_line, " \t", &ctx);
		char* a1 = strtok_s(nullptr, " \t", &ctx);
		char* a2 = strtok_s(nullptr, " \t", &ctx);
		char* a3 = strtok_s(nullptr, " \t", &ctx);
		char* a4 = strtok_s(nullptr, " \t", &ctx);
		if (!c) return;
		if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "load") && a2) {
			// gotham load <file in logs\> [limit] [y offset m]: the file's origin goes to the hero's feet
			HeroXf xf;
			if (!ReadHero(xf)) {
				Log("  no hero");
				return;
			}
			std::string fn = a2;
			double yoff = a4 ? atof(a4) : 0.0;
			gotham::LoadFile(g_logDir + L"\\" + std::wstring(fn.begin(), fn.end()), xf.pos[0], xf.pos[1] + yoff, xf.pos[2], a3 ? atoi(a3) : 0, 0.0f);
			Log("  world %p (query world %p), zone lock %s, static motion props %p", hk::World(), hk::QueryWorld(), hk::ZoneLock() ? "ready" : "NOT initialized",
				hk::World() ? hk::StaticMotionProperties(hk::World()) : nullptr);
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "tiles") && a2) {
			// gotham tiles <file.awt in logs\> [y offset m] [shape tag]: compressed-mesh tiles, origin at the hero's feet
			HeroXf xf;
			if (!ReadHero(xf)) {
				Log("  no hero");
				return;
			}
			if (a4) gotham::g_tileShapeTag = static_cast<uint16_t>(strtoul(a4, nullptr, 0));
			std::string fn = a2;
			gotham::LoadTiles(g_logDir + L"\\" + std::wstring(fn.begin(), fn.end()), xf.pos[0], xf.pos[1] + (a3 ? atof(a3) : 0.0), xf.pos[2]);
		} else if (!_stricmp(c, "gotham") && a1 && (!_stricmp(a1, "hide") || !_stricmp(a1, "show"))) {
			// hide: Gotham bodies stay in the world but no query sees them (also turns the NY filter off)
			gotham::g_hidden = !_stricmp(a1, "hide");
			if (gotham::g_hidden) gotham::g_filterOn = false;
			hints::g_hideOurs = gotham::g_hidden.load();
			if (gotham::g_hidden) hints::g_onlyOurs = false;
			Log("  Gotham %s", gotham::g_hidden ? "HIDDEN (no query sees it, New York normal)" : "visible");
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "filter") && a2) {
			gotham::g_filterOn = !_stricmp(a2, "on");
			hints::g_onlyOurs = gotham::g_filterOn.load();
			if (gotham::g_filterOn) gotham::g_hidden = false, hints::g_hideOurs = false;
			Log("  New York filter %s (filter vtable %s)", gotham::g_filterOn ? "ON: queries only see Gotham" : "off", gotham::g_patchedVtable ? "patched" : "NOT patched yet");
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "status")) {
			HeroXf xf;
			bool   h = ReadHero(xf);
			Log("  physics steps seen %llu", static_cast<unsigned long long>(g_steps));
			Log("  shapes built %d, failed %d, bodies %d in %d systems, pending %zu; filter %s, calls %llu, rejected %llu, body shape offset %d; hero %.2f %.2f %.2f",
				gotham::g_shapesBuilt.load(), gotham::g_shapeFailures.load(), gotham::g_bodiesAdded.load(), gotham::g_systems.load(),
				gotham::g_pending.size() - gotham::g_nextPending, gotham::g_filterOn ? "ON" : "off", static_cast<unsigned long long>(gotham::g_filterCalls.load()),
				static_cast<unsigned long long>(gotham::g_filterRejected.load()), gotham::g_shapeOffset.load(), h ? xf.pos[0] : 0, h ? xf.pos[1] : 0, h ? xf.pos[2] : 0);
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "hints") && a2) {
			// gotham hints <file.awh in logs\>: swing hint boxes, placed at the last tiles/load origin
			std::string fn = a2;
			hints::Load(g_logDir + L"\\" + std::wstring(fn.begin(), fn.end()), gotham::g_place);
		} else if (!_stricmp(c, "hero") && a1 && !_stricmp(a1, "tp") && a2 && a3 && a4) {
			// hero tp dx dy dz (meters): the teleport the sky jump uses, for testing
			HeroXf xf;
			if (!ReadHero(xf)) return;
			g_tpTarget[0] = xf.pos[0] + atof(a2), g_tpTarget[1] = xf.pos[1] + atof(a3), g_tpTarget[2] = xf.pos[2] + atof(a4);
			g_tpRequest = 2;
		} else if (!_stricmp(c, "cam") && a1) {
			// cam off | on | recal: the camera steering toward the host camera's yaw
			if (!_stricmp(a1, "recal")) g_steerRecal = true;
			else g_steerEnabled = !_stricmp(a1, "on");
			Log("  camera steering %s (sign %d, %.0f counts/rad)", !_stricmp(a1, "recal") ? "recalibrating" : g_steerEnabled ? "on" : "off", g_sign,
				g_countsPerRad);
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "unhints")) {
			hints::Queue({ 3, {}, {} });
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "stream") && a2) {
			RunStreamCommand(a2, a3, a4, ctx);
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "qstats")) {
			Log("  hints: %d in the database, %llu hint queries, %llu Gotham hints returned, %llu New York hints dropped", hints::g_inserted.load(),
				static_cast<unsigned long long>(hints::g_queries.exchange(0)), static_cast<unsigned long long>(hints::g_returnedOurs.exchange(0)),
				static_cast<unsigned long long>(hints::g_droppedNy.exchange(0)));
			// filter calls since the last qstats, by entry point and query type
			Log("  slot 4 rule %s; rejected by query type 1/2/4: %llu / %llu / %llu", gotham::g_slot4On ? "on" : "off",
				static_cast<unsigned long long>(gotham::g_shapeRejected[1].exchange(0)), static_cast<unsigned long long>(gotham::g_shapeRejected[2].exchange(0)),
				static_cast<unsigned long long>(gotham::g_shapeRejected[4].exchange(0)));
			gotham::LogQueryStats();
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "slot4") && a2) {
			gotham::g_slot4On = !_stricmp(a2, "on");
			Log("  slot 4 (shape-level) rule %s", gotham::g_slot4On ? "on" : "off");
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "perframe") && a2) {
			gotham::g_perFrame = atoi(a2);
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "surface")) {
			// gotham surface [hex]: the collision filter info (surface categories, havok.h) of Gotham's bodies
			if (a2) {
				uint32_t info = static_cast<uint32_t>(strtoul(a2, nullptr, 16));
				int      n = gotham::SetFilterInfo(info);
				Log("  Gotham bodies now carry filter info %08x (%d live bodies rewritten)", info, n);
			} else {
				Log("  Gotham bodies carry filter info %08x (categories %03x)", gotham::g_filterInfo.load(), gotham::g_filterInfo.load() & hk::kCategoryMask);
			}
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "presets")) {
			gotham::LogPresets();
		} else if (!_stricmp(c, "gotham") && a1 && !_stricmp(a1, "hitflags")) {
			// gotham hitflags [s]: which surface categories hits carry, for a few seconds (logged at the end)
			double s = a2 ? atof(a2) : 3.0;
			gotham::g_hitSampleUntil = GetTickCount64() + static_cast<ULONGLONG>(s * 1000.0);
			Log("  sampling hits for %.1f s", s);
		} else if (!_stricmp(c, "zip")) {
			float cam[16];
			bool  haveCam = ReadCamera(cam);
			zip::Command(a1, a2, a3, cam, haveCam);
		} else if (!_stricmp(c, "cap")) {
			capture::Command(a1, a2);  // Spider-Man's frame for Arkham (capture.h)
		} else if (!_stricmp(c, "ptr") && a1) {
			uintptr_t a = !_strnicmp(a1, "exe+", 4) ? g_exe + strtoull(a1 + 4, nullptr, 16) : strtoull(a1, nullptr, 16);
			Log("  [%s] = %p", a1, reinterpret_cast<void*>(ReadOr<uintptr_t>(a, 0)));
		} else if (!_stricmp(c, "mem") && a1) {
			uintptr_t a = !_strnicmp(a1, "exe+", 4) ? g_exe + strtoull(a1 + 4, nullptr, 16) : strtoull(a1, nullptr, 16);
			size_t    n = a2 ? strtoull(a2, nullptr, 16) : 0x80;
			std::vector<uint8_t> b(n);
			if (!SafeRead(b.data(), reinterpret_cast<void*>(a), n)) {
				Log("  unreadable");
				return;
			}
			for (size_t o = 0; o < n; o += 16) {
				char line[200];
				int  p = snprintf(line, sizeof(line), "  +%04zx:", o);
				for (size_t i = 0; i < 16 && o + i < n; ++i) p += snprintf(line + p, sizeof(line) - p, " %02x", b[o + i]);
				Log("%s", line);
			}
		} else {
			Log("  unknown command");
		}
		(void)a3;
	}

	void InstallHooks()
	{
		g_realGetProcAddress = reinterpret_cast<GetProcAddressFn>(IatHook("KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&GetProcAddressDetour)));
		g_realGetForeground = reinterpret_cast<HwndFn>(IatHook("USER32.dll", "GetForegroundWindow", reinterpret_cast<void*>(&GetForegroundDetour)));
		g_realGetFocus = reinterpret_cast<HwndFn>(IatHook("USER32.dll", "GetFocus", reinterpret_cast<void*>(&GetFocusDetour)));
		g_realClipCursor = reinterpret_cast<ClipCursorFn>(IatHook("USER32.dll", "ClipCursor", reinterpret_cast<void*>(&ClipCursorDetour)));
		g_realSetCursorPos = reinterpret_cast<SetCursorPosFn>(IatHook("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&SetCursorPosDetour)));
		g_realRegQueryW = reinterpret_cast<RegQueryValueExWFn>(IatHook("ADVAPI32.dll", "RegQueryValueExW", reinterpret_cast<void*>(&RegQueryWDetour)));
		g_realRegQueryA = reinterpret_cast<RegQueryValueExAFn>(IatHook("ADVAPI32.dll", "RegQueryValueExA", reinterpret_cast<void*>(&RegQueryADetour)));
		g_realRegGetA = reinterpret_cast<RegGetValueAFn>(IatHook("ADVAPI32.dll", "RegGetValueA", reinterpret_cast<void*>(&RegGetADetour)));
		Log("settings hooks: RegQueryValueExW %s, RegQueryValueExA %s, RegGetValueA %s (VirtualPad=%d)", g_realRegQueryW ? "ok" : "MISSING",
			g_realRegQueryA ? "ok" : "MISSING", g_realRegGetA ? "ok" : "MISSING", g_virtualPadSetting);
		g_realGetRawInputData = reinterpret_cast<GetRawInputDataFn>(IatHook("USER32.dll", "GetRawInputData", reinterpret_cast<void*>(&GetRawInputDataDetour)));
		Log("raw input hook: GetRawInputData %s", g_realGetRawInputData ? "ok" : "MISSING");
		gotham::Init();
		g_origPreCollide = reinterpret_cast<PreCollideFn>(InlineHook(g_exe + kPreCollide, kPreCollideSig, sizeof(kPreCollideSig), reinterpret_cast<void*>(&PreCollideDetour)));
		g_origCastRay = reinterpret_cast<Fn6>(InlineHook(g_exe + kCastRay, kCastRaySig, sizeof(kCastRaySig), reinterpret_cast<void*>(&CastRayDetour)));
		g_origCastShape = reinterpret_cast<Fn6>(InlineHook(g_exe + kCastShape, kCastRaySig, sizeof(kCastRaySig), reinterpret_cast<void*>(&CastShapeDetour)));
		g_origClosest = reinterpret_cast<Fn6>(InlineHook(g_exe + kClosestPoints, kCastRaySig, sizeof(kCastRaySig), reinterpret_cast<void*>(&ClosestDetour)));
		Log("phase 2 hooks: hknpWorld pre-collide %s, castRay %s, castShape %s, getClosestPoints %s", g_origPreCollide ? "ok" : "FAILED",
			g_origCastRay ? "ok" : "FAILED", g_origCastShape ? "ok" : "FAILED", g_origClosest ? "ok" : "FAILED");
		hints::Install();
		zip::g_heroLocal = &g_heroLocal;
		zip::Install();
		int captured = CaptureThisOnVtable(g_exe + kHeroLocalVtable, kHeroLocalSlots, &g_heroLocal);
		int camSlots = CaptureThisOnVtable(g_exe + kHeroCameraManagerVtable, kHeroCameraManagerSlots, &g_heroCamera);
		Log("camera manager capture: %d/%d slots", camSlots, kHeroCameraManagerSlots);
		Log("hooks: GetProcAddress %s, GetForegroundWindow %s, GetFocus %s, ClipCursor %s, SetCursorPos %s, HeroLocal vtable %d/%d slots",
			g_realGetProcAddress ? "ok" : "MISSING", g_realGetForeground ? "ok" : "MISSING", g_realGetFocus ? "ok" : "MISSING",
			g_realClipCursor ? "ok" : "MISSING", g_realSetCursorPos ? "ok" : "MISSING", captured, kHeroLocalSlots);
	}

	// ---- snapshots: the hero and his camera from the same Spider-Man frame ------------------------------
	// Spider-Man moves the hero, then the camera, then renders; sampling at an arbitrary moment can pair a
	// new hero with last frame's camera (Batman then shakes against the view). So the transforms are
	// watched every millisecond and a pair is taken once both have stopped changing for 2 ms - the frame's
	// updates are done - stamped with the moment the frame's first change was seen (QPC, the same clock in
	// both processes: the host interpolates between frames with it).
	struct Snapshot
	{
		HeroXf  hero;
		float   cam[16];
		bool    camOk;
		int64_t qpc;
	};
	SRWLOCK  g_snapLock = SRWLOCK_INIT;
	Snapshot g_snap{};
	bool     g_snapValid = false;

	bool ReadCamera(float a_out[16])
	{
		auto cm = reinterpret_cast<uintptr_t>(g_heroCamera);
		if (!cm) return false;
		float a[16], b[16];
		for (int i = 0; i < 4; ++i) {
			if (!SafeRead(a, reinterpret_cast<const void*>(cm + kCameraMatrix), sizeof(a)) || !SafeRead(b, reinterpret_cast<const void*>(cm + kCameraMatrix), sizeof(b)))
				return false;
			if (memcmp(a, b, sizeof(a)) == 0) break;
		}
		for (int i = 0; i < 16; ++i) {
			if (!std::isfinite(a[i])) return false;
		}
		memcpy(a_out, a, sizeof(a));
		return true;
	}

	bool TakeSnapshot(Snapshot& a_out)
	{
		AcquireSRWLockShared(&g_snapLock);
		bool ok = g_snapValid;
		if (ok) a_out = g_snap;
		ReleaseSRWLockShared(&g_snapLock);
		return ok;
	}

	// The camera a captured frame was presented with, in the frame GuestState uses (capture.h).
	bool CaptureCamera(double a_pos[3], float a_rot[9])
	{
		float c[16];
		if (!ReadCamera(c)) return false;
		for (int r = 0; r < 3; ++r) {
			for (int k = 0; k < 3; ++k) a_rot[r * 3 + k] = c[r * 4 + k];
		}
		for (int i = 0; i < 3; ++i) a_pos[i] = gotham::g_anchorValid ? c[12 + i] - gotham::g_place[i] + gotham::g_anchorGuest[i] : c[12 + i];
		return true;
	}

	// What a `cap dump` adds: the hero, the raw camera, and the camera manager's bytes (for finding the FOV).
	void CaptureDumpExtra(FILE* a_f)
	{
		HeroXf xf;
		if (ReadHero(xf)) {
			fprintf(a_f, "hero pos %g %g %g rows %g %g %g / %g %g %g / %g %g %g half %g %g %g\n", xf.pos[0], xf.pos[1], xf.pos[2], xf.rows[0][0], xf.rows[0][1],
				xf.rows[0][2], xf.rows[1][0], xf.rows[1][1], xf.rows[1][2], xf.rows[2][0], xf.rows[2][1], xf.rows[2][2], xf.halfExtents[0], xf.halfExtents[1],
				xf.halfExtents[2]);
		}
		float c[16];
		if (ReadCamera(c)) {
			fprintf(a_f, "camera");
			for (float v : c) fprintf(a_f, " %g", v);
			fprintf(a_f, "\n");
		}
		auto cm = reinterpret_cast<uintptr_t>(g_heroCamera);
		std::vector<uint8_t> b(0x2000);
		if (cm && SafeRead(b.data(), reinterpret_cast<const void*>(cm), b.size())) {
			if (FILE* f = _wfopen((g_logDir + L"\\capture\\camman.bin").c_str(), L"wb")) {
				fwrite(b.data(), 1, b.size(), f);
				fclose(f);
			}
			fprintf(a_f, "camman %p (0x2000 bytes in camman.bin, matrix at +0x%x)\n", reinterpret_cast<void*>(cm), kCameraMatrix);
		}
	}

	DWORD WINAPI SnapshotThread(void*)
	{
		// 1 ms sleeps need the 1 ms timer (the real winmm's; this DLL's own export only forwards to it)
		if (HMODULE w = LoadLibraryW(L"C:\\Windows\\System32\\winmm.dll")) {
			if (auto f = reinterpret_cast<UINT(WINAPI*)(UINT)>(GetProcAddress(w, "timeBeginPeriod"))) f(1);
		}
		HeroXf  lastH{};
		float   lastC[16] = {};
		bool    pending = false;
		int     stable = 0;
		int64_t changeQpc = 0;
		for (;;) {
			Sleep(1);
			HeroXf h;
			if (!ReadHero(h)) {
				AcquireSRWLockExclusive(&g_snapLock);
				g_snapValid = false;
				ReleaseSRWLockExclusive(&g_snapLock);
				pending = false;
				continue;
			}
			float c[16] = {};
			bool  cok = ReadCamera(c);
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			if (memcmp(&h, &lastH, sizeof(h)) != 0 || memcmp(c, lastC, sizeof(c)) != 0) {
				if (!pending) pending = true, changeQpc = now.QuadPart;
				lastH = h;
				memcpy(lastC, c, sizeof(c));
				stable = 0;
				continue;
			}
			bool first = false;
			AcquireSRWLockShared(&g_snapLock);
			first = !g_snapValid;
			ReleaseSRWLockShared(&g_snapLock);
			if ((pending && ++stable >= 2) || first) {
				Snapshot s{ h, {}, cok, pending ? changeQpc : now.QuadPart };
				memcpy(s.cam, c, sizeof(c));
				AcquireSRWLockExclusive(&g_snapLock);
				g_snap = s;
				g_snapValid = true;
				ReleaseSRWLockExclusive(&g_snapLock);
				pending = false;
			}
		}
	}

	// ---- worker: link, publishing, focus state ---------------------------------------------------------
	DWORD WINAPI Worker(void*)
	{
		InitKeyboardInjection();  // registry + device list: not from DllMain
		while (!g_link.Open()) Sleep(1000);
		g_link.Header()->guestPid = GetCurrentProcessId();
		g_ring = Ring(g_link.Base() + proto::kOffCollisionRing, proto::kCollisionRingBytes);
		g_ring.Discard();  // anything still in there was meant for an earlier Spider-Man
		g_ringReady = true;
		capture::g_state = g_link.Capture();
		Log("link open (%s), collision ring ready", proto::kMappingNameA);

		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		uint64_t frame = 0;
		double   lastPos[3] = {};
		float    lastVel[3] = {};
		int64_t  lastQpc = 0;
		bool     wasInWorld = false, wasFake = false;
		struct Sample
		{
			ULONGLONG ms;
			double    pos[3];
		};
		Sample    grounded[8] = {};  // spots on Gotham where the hero stood still (for rescues)
		unsigned  groundedNext = 0, lowReadings = 0;
		double    lastSampleY = 0.0, lastSampleXZ[2] = {};
		ULONGLONG lastSampleMs = 0, lastRescueMs = 0, stillSince = 0;
		for (uint64_t loop = 0;; ++loop) {
			Sleep(4);
			if (g_window && !IsWindow(g_window)) {
				Log("game window %p is gone", g_window);
				g_window = nullptr;
			}
			if (!g_window && loop % 64 == 0) {
				EnumWindows(FindGameWindow, 0);
				if (g_window) {
					g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcDetour)));
					Log("game window %p (GameNxApp) subclassed", g_window);
					if (g_fakeFocus) PostActivation();
					capture::g_windowReady = true;
				}
			}
			bool hostAlive = Link::Alive(g_link.Header()->hostHeartbeatMs);
			g_fakeFocus = g_forceFocus || hostAlive;
			if (g_fakeFocus != wasFake) {
				Log("fake focus %s (%s)", g_fakeFocus ? "ON" : "off", g_forceFocus ? "ForceFocus=1" : hostAlive ? "host linked" : "no host");
				wasFake = g_fakeFocus;
				if (g_fakeFocus) PostActivation();
			}

			if (g_fakeFocus) HoldWindowActive();
			if (g_realGetRawInputData) PumpPadToKeys();

			HeroXf xf;
			bool   inWorld = ReadHero(xf);
			if (inWorld != wasInWorld) {
				Log(inWorld ? "hero found (HeroLocal %p) at %.2f %.2f %.2f" : "hero lost", g_heroLocal, xf.pos[0], xf.pos[1], xf.pos[2]);
				wasInWorld = inWorld;
			}
			zip::LoadIfChanged();  // Arkham's grapple ledges (zip.h)
			zip::KeepTm(inWorld);
			if (zip::g_pinState.load() == 1 && inWorld) {  // a new perch: find the copies of his position to pin
				const float p[3] = { xf.pos[0], xf.pos[1], xf.pos[2] };
				zip::SetPin(FindPositionCopies(p), p);
			}
			if (ULONGLONG until = gotham::g_hitSampleUntil.load(); until && GetTickCount64() > until) {
				gotham::g_hitSampleUntil = 0;
				gotham::LogHitFlags();
			}

			// teleports asked for by commands (they need to wait a few frames, the pump thread can't)
			if (int req = g_tpRequest.exchange(0); req && inWorld) {
				double         target[3] = { g_tpTarget[0], g_tpTarget[1], g_tpTarget[2] };
				TeleportResult r = Teleport(target);
				if (req == 1) {
					if (r.ok) {
						gotham::g_anchorValid = true;  // from now on Batman follows him
						g_skyActive = gotham::g_sky > 0.0;
						Log("jumped onto Gotham %s", g_skyActive ? "in the sky" : "");
					} else {
						g_jumpFailed = true;
					}
				}
				inWorld = ReadHero(xf);
			}

			// In the sky a hero who drops through a gap in Gotham falls toward New York, a kilometer down:
			// put him back on ground he stood on (a spot where he was still for half a second - not one in
			// mid-swing), or on the start spot. Two clean readings in a row first: a respawn or a load can
			// report nonsense for a moment.
			if (inWorld && g_skyActive && gotham::g_anchorValid) {
				ULONGLONG t = GetTickCount64();
				if (xf.pos[1] > gotham::g_place[1] - 80.0) {
					if (t - lastSampleMs >= 250) {
						lastSampleMs = t;
						bool still = std::fabs(xf.pos[1] - lastSampleY) < 0.15 && std::hypot(xf.pos[0] - lastSampleXZ[0], xf.pos[2] - lastSampleXZ[1]) < 2.5;
						if (still && stillSince == 0) stillSince = t;
						if (!still) stillSince = 0;
						if (stillSince && t - stillSince >= 500) grounded[groundedNext++ % 8] = { t, { xf.pos[0], xf.pos[1], xf.pos[2] } };
						lastSampleY = xf.pos[1], lastSampleXZ[0] = xf.pos[0], lastSampleXZ[1] = xf.pos[2];
					}
					lowReadings = 0;
				} else if (xf.pos[1] < gotham::g_place[1] - 150.0 && ++lowReadings >= 2 && t - lastRescueMs > 3000 && t - g_lastTeleportMs > 2000) {
					lastRescueMs = t;
					const Sample* best = nullptr;  // the newest grounded spot
					for (auto& s : grounded) {
						if (s.ms && (!best || s.ms > best->ms)) best = &s;
					}
					double target[3] = { gotham::g_place[0], gotham::g_place[1] + 1.0, gotham::g_place[2] };
					if (best) target[0] = best->pos[0], target[1] = best->pos[1] + 1.0, target[2] = best->pos[2];
					TeleportResult r = Teleport(target);
					++g_rescues;
					Log("rescue %d: fell through Gotham (y %.1f, Gotham at %.1f), put back %s at (%.1f %.1f %.1f): %s", g_rescues.load(), xf.pos[1],
						gotham::g_place[1], best ? "where he last stood" : "on the start spot", target[0], target[1], target[2], r.ok ? "ok" : "FAILED");
					inWorld = ReadHero(xf);
				}
			}
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			proto::GuestState gs{};
			gs.flags = (inWorld ? proto::kGuestInWorld : 0) | (g_fakeFocus ? proto::kGuestFocusFaked : 0) |
				(gotham::g_anchorValid ? proto::kGuestAnchored : 0) | (g_skyActive ? proto::kGuestSky : 0) | (g_jumpFailed ? proto::kGuestJumpFailed : 0);
			gs.frame = ++frame;
			gs.qpc = now.QuadPart;
			gs.tilesHeld = static_cast<uint32_t>(gotham::g_tilesHeld.load(std::memory_order_relaxed));
			gs.tilesQueued = static_cast<uint32_t>(gotham::g_tilesQueued.load(std::memory_order_relaxed));
			uint64_t heapFree = gotham::g_heapFree.load(std::memory_order_relaxed);  // sampled on the pump thread
			gs.heapFreeMB = heapFree != ~0ull ? static_cast<uint32_t>(heapFree >> 20) : 0xFFFFFFFF;
			gs.heapUsedMB = static_cast<uint32_t>(gotham::g_heapUsed.load(std::memory_order_relaxed) >> 20);
			gs.rescues = static_cast<uint32_t>(g_rescues.load());
			gs.buildsRefused = static_cast<uint32_t>(gotham::g_buildsRefused.load());
			if (inWorld) {
				gotham::g_heroX.store(xf.pos[0], std::memory_order_relaxed);  // build order: nearest tile first
				gotham::g_heroZ.store(xf.pos[2], std::memory_order_relaxed);
				gotham::g_heroKnown.store(true, std::memory_order_relaxed);
			}
			Snapshot snap;
			bool     haveSnap = inWorld && TakeSnapshot(snap);
			if (haveSnap) {
				// the hero and his camera from one Spider-Man frame, stamped with that frame's time
				const HeroXf& sx = snap.hero;
				gs.qpc = snap.qpc;
				for (int i = 0; i < 3; ++i) {
					gs.heroPos[i] = gotham::g_anchorValid ? sx.pos[i] - gotham::g_place[i] + gotham::g_anchorGuest[i] : sx.pos[i];
					for (int k = 0; k < 3; ++k) gs.heroRot[i * 3 + k] = sx.rows[i][k];
				}
				// velocity between Spider-Man frames (a frame is a new snapshot time)
				if (snap.qpc != lastQpc) {
					double dt = lastQpc ? static_cast<double>(snap.qpc - lastQpc) / freq.QuadPart : 0.0;
					if (dt > 0.0 && dt < 0.25) {
						for (int i = 0; i < 3; ++i) lastVel[i] = static_cast<float>((gs.heroPos[i] - lastPos[i]) / dt);
					}
					for (int i = 0; i < 3; ++i) lastPos[i] = gs.heroPos[i];
					lastQpc = snap.qpc;
				} else if (static_cast<double>(now.QuadPart - lastQpc) / freq.QuadPart > 0.25) {
					lastVel[0] = lastVel[1] = lastVel[2] = 0.0f;  // really standing still
				}
				for (int i = 0; i < 3; ++i) gs.heroVel[i] = lastVel[i];
				gs.heroHeight = sx.halfExtents[1] > 0.5f && sx.halfExtents[1] < 5.0f ? sx.halfExtents[1] : 1.78f;
				// Spider-Man's camera (HeroCameraManager +0x744: rows side, up, forward, position): the host
				// shows it as Arkham's view.
				if (snap.camOk) {
					for (int r = 0; r < 3; ++r) {
						for (int k = 0; k < 3; ++k) gs.camRot[r * 3 + k] = snap.cam[r * 4 + k];
					}
					for (int i = 0; i < 3; ++i) gs.camPos[i] = gotham::g_anchorValid ? snap.cam[12 + i] - gotham::g_place[i] + gotham::g_anchorGuest[i] : snap.cam[12 + i];
					// the follow camera's lens (vertical, degrees): Arkham uses it too, so the captured hero fits
					gs.camFovYDeg = gotham::g_anchorValid ? capture::FovYDeg() : 0.0f;
				}
			} else {
				inWorld = false;
				gs.flags &= ~proto::kGuestInWorld;
			}
			// zip to point: the ledge the camera looks at, about 30 times a second (zip.h; L2 + R2 is seen
			// where the game reads the pad)
			if (loop % 8 == 0) {
				if (haveSnap && snap.camOk) {
					double hero[3] = { snap.hero.pos[0], snap.hero.pos[1], snap.hero.pos[2] };
					zip::UpdateTarget(snap.cam, hero, g_skyActive && gotham::g_anchorValid);
				} else {
					float  none[16] = {};
					double origin[3] = {};
					zip::UpdateTarget(none, origin, false);
				}
			}
			zip::Target zt = zip::Current();
			gs.zipFlags = zip::Flags();
			gs.zipEdges = static_cast<uint32_t>(zip::g_edgeCount.load());
			for (int i = 0; i < 3; ++i) gs.zipTarget[i] = zt.abs[i];
			gs.zipStarted = zip::g_started.load();
			gs.zipFailed = zip::g_failed.load();
			SeqWrite(g_link.Guest(), gs);
			g_link.Header()->guestHeartbeatMs = GetTickCount64();
			// Spider-Man's frames go to Arkham while he is on Gotham in the sky and Arkham is there (capture.h)
			capture::g_autoWant = inWorld && g_skyActive && gotham::g_anchorValid && hostAlive;
			if (inWorld && loop % 2500 == 0) {
				Log("hero at %.2f %.2f %.2f m; XInput polls %llu, virtual pad served %llu, injected keys read %llu", gs.heroPos[0], gs.heroPos[1], gs.heroPos[2],
					static_cast<unsigned long long>(g_padPolls.load()), static_cast<unsigned long long>(g_padServed.load()),
					static_cast<unsigned long long>(g_fakeServed.load()));
			}
		}
	}
}

BOOL APIENTRY DllMain(HMODULE a_self, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_self);
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		if (!wcsstr(exe, L"Spider-Man.exe")) return TRUE;
		g_self = a_self;
		InitLog(a_self, L"sm_guest");
		g_forceFocus = IniInt(a_self, L"ForceFocus", 0);
		g_virtualPadSetting = IniInt(a_self, L"VirtualPad", 1);
		Log("ArkWeb guest loaded (exe %p), ForceFocus=%d", reinterpret_cast<void*>(g_exe), g_forceFocus);
		InstallHooks();
		capture::g_readCamera = &CaptureCamera;
		capture::g_dumpExtra = &CaptureDumpExtra;
		CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
		CreateThread(nullptr, 0, SnapshotThread, nullptr, 0, nullptr);
		CreateThread(nullptr, 0, capture::Thread, nullptr, 0, nullptr);
	}
	return TRUE;
}
