// Arkham Knight (UE3: Z up, X forward, Y right, left-handed, 1 UU = 1 cm, Rotator 65536 = 360 deg)
// <-> Spider-Man (Y up, meters, actor matrix rows = side, up, forward).
//
// The two engines disagree on handedness only if Spider-Man is right-handed. kGuestRightHanded
// picks the mapping that keeps the physical world unmirrored; it is confirmed in-game by the
// strafe test (tools/fake_arkham.py strafe: stick right must move the hero along -side if the
// guest is right-handed, see PHASE1.md).
#pragma once

#include <cmath>

namespace arkweb::coords
{
	inline constexpr bool   kGuestRightHanded = true;
	inline constexpr double kUnitsPerMeter = 100.0;  // Batman: 190 UU tall; Spider-Man: 1.78 m
	inline constexpr double kPi = 3.14159265358979323846;

	// Where Gotham's origin sits in Spider-Man's world (meters, guest space). Phase 2 may move it
	// to keep Gotham inside the guest's playable bounds.
	inline double g_offset[3] = { 0.0, 0.0, 0.0 };

	struct V3
	{
		double x, y, z;
	};

	// Directions (no scale, no offset).
	inline V3 HostDirToGuest(V3 a_h)
	{
		// guest Y = host Z (up). Right-handed guest: swap host Y into guest Z (a reflection,
		// which is exactly what a left->right-handed change needs). Left-handed guest: rotate.
		return kGuestRightHanded ? V3{ a_h.x, a_h.z, a_h.y } : V3{ a_h.x, a_h.z, -a_h.y };
	}

	inline V3 GuestDirToHost(V3 a_g)
	{
		return kGuestRightHanded ? V3{ a_g.x, a_g.z, a_g.y } : V3{ a_g.x, -a_g.z, a_g.y };
	}

	inline V3 HostPosToGuest(V3 a_uu)
	{
		V3 d = HostDirToGuest({ a_uu.x / kUnitsPerMeter, a_uu.y / kUnitsPerMeter, a_uu.z / kUnitsPerMeter });
		return { d.x + g_offset[0], d.y + g_offset[1], d.z + g_offset[2] };
	}

	inline V3 GuestPosToHost(V3 a_m)
	{
		V3 h = GuestDirToHost({ a_m.x - g_offset[0], a_m.y - g_offset[1], a_m.z - g_offset[2] });
		return { h.x * kUnitsPerMeter, h.y * kUnitsPerMeter, h.z * kUnitsPerMeter };
	}

	// UE3 yaw (Rotator units) of a guest forward vector.
	inline int GuestForwardToHostYaw(V3 a_fwd)
	{
		V3     h = GuestDirToHost(a_fwd);
		double rad = std::atan2(h.y, h.x);
		return static_cast<int>(std::lround(rad * 32768.0 / kPi)) & 0xFFFF;
	}

	// UE3 pitch (Rotator units, up positive) of a guest forward vector.
	inline int GuestForwardToHostPitch(V3 a_fwd)
	{
		V3     h = GuestDirToHost(a_fwd);
		double rad = std::atan2(h.z, std::sqrt(h.x * h.x + h.y * h.y));
		return static_cast<int>(std::lround(rad * 32768.0 / kPi)) & 0xFFFF;
	}

	// Guest yaw convention used in the protocol: radians about +Y, 0 = facing +Z, measured so
	// that forward = (sin yaw, 0, cos yaw).
	inline double GuestYawOfForward(V3 a_fwd) { return std::atan2(a_fwd.x, a_fwd.z); }

	inline V3 GuestForwardOfYaw(double a_yaw) { return { std::sin(a_yaw), 0.0, std::cos(a_yaw) }; }

	inline V3 HostForwardOfYaw(int a_yaw)
	{
		double r = (a_yaw & 0xFFFF) * kPi / 32768.0;
		return { std::cos(r), std::sin(r), 0.0 };
	}

	struct HostRotator
	{
		int pitch, yaw, roll;  // UE3 Rotator units (65536 = 360 deg)
	};

	// A guest camera basis (forward and up rows) as a UE3 Rotator, roll included. UE3's rotation
	// matrix (FRotationMatrix) has X = (CP*CY, CP*SY, SP) and Z = CR*Z0 + SR*Y0 with the roll-free
	// axes Y0 = (-SY, CY, 0), Z0 = (-SP*CY, -SP*SY, CP); so roll = atan2(up.Y0, up.Z0).
	inline HostRotator GuestBasisToHostRotator(V3 a_fwd, V3 a_up)
	{
		V3     f = GuestDirToHost(a_fwd), u = GuestDirToHost(a_up);
		double yaw = std::atan2(f.y, f.x);
		double pitch = std::atan2(f.z, std::sqrt(f.x * f.x + f.y * f.y));
		double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
		double uy0 = u.x * -sy + u.y * cy, uz0 = u.x * -sp * cy + u.y * -sp * sy + u.z * cp;
		double roll = std::atan2(uy0, uz0);
		auto   units = [](double a_rad) { return static_cast<int>(std::lround(a_rad * 32768.0 / kPi)); };
		return { units(pitch), units(yaw), units(roll) };
	}
}
