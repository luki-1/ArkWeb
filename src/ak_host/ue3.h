// Minimal Rocksteady-UE3 object access for BatmanAK.exe (Steam, 2026). Layouts and addresses are
// from ArkWeb/recon/PHASE0b.md; all RVAs are relative to the exe.
#pragma once

#include "../common/util.h"

#include <string>
#include <vector>

namespace arkweb::ue3
{
	constexpr uintptr_t kGNames = 0x3a208b8;       // TArray<FNameEntry*>
	constexpr uintptr_t kGObjData = 0x340cbe4;     // UObject*[785000], inline (pack 4)
	constexpr uintptr_t kGObjNum = 0x3a09f28;      // int
	constexpr int       kGObjMax = 785000;
	constexpr uintptr_t kProcessEvent = 0xf90cd0;  // UObject::ProcessEvent(UFunction*, void* parms, void* result)
	constexpr uint8_t   kProcessEventSig[18] = { 0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8D, 0x6C, 0x24, 0x20 };

	// UObject (pack 4)
	constexpr int kObjOuter = 0x34, kObjName = 0x3C, kObjClass = 0x44;
	// Actor / Pawn / Controller fields (recon/ak_sdk.txt)
	constexpr int kActorLocation = 0xC4, kActorRotation = 0xD0, kActorPhysics = 0x11B;
	constexpr int kControllerPawn = 0x29C;
	constexpr int kPawnCylinder = 0x384, kCylinderHeight = 0x21C;

	enum EPhysics : uint8_t
	{
		PHYS_None = 0,
		PHYS_Walking = 1,
		PHYS_Falling = 2,
	};

	struct FVector
	{
		float x, y, z;
	};
	struct FRotator
	{
		int pitch, yaw, roll;
	};

	using ProcessEventFn = void (*)(void* a_obj, void* a_fn, void* a_parms, void* a_result);

	struct TArrayRaw
	{
		uintptr_t data;
		int32_t   num, max;
	};

	inline std::string ReadStr(uintptr_t a_p, bool a_wide)
	{
		std::string out;
		for (int i = 0; i < 1024; ++i) {
			uint16_t c = 0;
			bool     ok = a_wide ? Read(a_p + i * 2ull, c) : Read(a_p + i, *reinterpret_cast<uint8_t*>(&c));
			if (!ok || c == 0) break;
			out.push_back(c < 128 ? static_cast<char>(c) : '?');
		}
		return out;
	}

	inline int NameCount()
	{
		TArrayRaw a{};
		Read(g_exe + kGNames, a);
		return a.num;
	}

	// FNameEntry: u32 index<<2|flags, hashNext (+4), name at +0xC (flags&1 UTF-16 inline, flags&2
	// pointer to ANSI, else ANSI inline).
	inline std::string NameOf(int32_t a_index)
	{
		TArrayRaw a{};
		Read(g_exe + kGNames, a);
		uintptr_t e = 0;
		uint32_t  idx = 0;
		if (a_index < 0 || a_index >= a.num || !Read(a.data + a_index * 8ull, e) || !e || !Read(e, idx)) return {};
		if (idx & 2) return ReadStr(ReadOr<uintptr_t>(e + 0xC, 0), false);
		return ReadStr(e + 0xC, (idx & 1) != 0);
	}

	inline int32_t FindName(const char* a_s)
	{
		for (int i = 0, n = NameCount(); i < n; ++i) {
			if (NameOf(i) == a_s) return i;
		}
		return -1;
	}

	inline int ObjCount()
	{
		int n = ReadOr<int32_t>(g_exe + kGObjNum, 0);
		return n < 0 ? 0 : n > kGObjMax ? kGObjMax : n;
	}

	inline uintptr_t ObjAt(int a_i) { return ReadOr<uintptr_t>(g_exe + kGObjData + a_i * 8ull, 0); }
	inline int32_t   NameIndexOf(uintptr_t a_obj) { return ReadOr<int32_t>(a_obj + kObjName, -1); }
	inline uintptr_t OuterOf(uintptr_t a_obj) { return ReadOr<uintptr_t>(a_obj + kObjOuter, 0); }
	inline uintptr_t ClassOf(uintptr_t a_obj) { return ReadOr<uintptr_t>(a_obj + kObjClass, 0); }

	// "Engine.Actor.SetLocation" -> the object (outermost first, exact names).
	inline uintptr_t FindObject(const char* a_path)
	{
		std::vector<int32_t> parts;
		std::string s = a_path;
		for (size_t p = 0;;) {
			size_t d = s.find('.', p);
			int32_t n = FindName(s.substr(p, d == std::string::npos ? std::string::npos : d - p).c_str());
			if (n < 0) return 0;
			parts.push_back(n);
			if (d == std::string::npos) break;
			p = d + 1;
		}
		for (int i = 0, n = ObjCount(); i < n; ++i) {
			uintptr_t o = ObjAt(i);
			if (!o || NameIndexOf(o) != parts.back()) continue;
			uintptr_t cur = OuterOf(o);
			bool      ok = true;
			for (int k = static_cast<int>(parts.size()) - 2; k >= 0 && ok; --k, cur = OuterOf(cur)) {
				ok = cur && NameIndexOf(cur) == parts[k];
			}
			if (ok && !cur) return o;
		}
		return 0;
	}
}
