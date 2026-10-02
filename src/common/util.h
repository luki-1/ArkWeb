// Shared helpers for the ArkWeb DLLs: logging to ArkWeb\logs, crash-safe reads, an inline hook for
// verified position-independent prologues, IAT hooks and vtable-slot patches.
#pragma once

#include <windows.h>
#include <intrin.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace arkweb
{
	// ---- logging ------------------------------------------------------------------------------
	// Logs go to <LogDir>\<tag>.log. `arkweb.ini` next to the DLL may set `LogDir=` (default below).
	inline std::wstring g_logDir = L"H:\\SteamLibrary\\steamapps\\common\\Saints Row the Third\\ArkWeb\\logs";
	inline FILE*        g_logFile = nullptr;
	inline SRWLOCK      g_logLock = SRWLOCK_INIT;
	inline uintptr_t    g_exe = 0;

	inline std::wstring IniPath(HMODULE a_self)
	{
		wchar_t p[MAX_PATH];
		GetModuleFileNameW(a_self, p, MAX_PATH);
		if (wchar_t* s = wcsrchr(p, L'\\')) wcscpy_s(s + 1, MAX_PATH - (s + 1 - p), L"arkweb.ini");
		return p;
	}

	// [ArkWeb] Key=value from arkweb.ini next to the DLL.
	inline std::wstring IniString(HMODULE a_self, const wchar_t* a_key, const wchar_t* a_default)
	{
		wchar_t buf[512];
		GetPrivateProfileStringW(L"ArkWeb", a_key, a_default, buf, 512, IniPath(a_self).c_str());
		return buf;
	}

	inline int IniInt(HMODULE a_self, const wchar_t* a_key, int a_default)
	{
		return GetPrivateProfileIntW(L"ArkWeb", a_key, a_default, IniPath(a_self).c_str());
	}

	inline void Log(const char* a_fmt, ...)
	{
		char    buf[2048];
		va_list va;
		va_start(va, a_fmt);
		vsnprintf(buf, sizeof(buf), a_fmt, va);
		va_end(va);
		SYSTEMTIME t;
		GetLocalTime(&t);
		AcquireSRWLockExclusive(&g_logLock);
		if (g_logFile) {
			fprintf(g_logFile, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
			fflush(g_logFile);
		}
		ReleaseSRWLockExclusive(&g_logLock);
	}

	inline void InitLog(HMODULE a_self, const wchar_t* a_name)
	{
		g_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		g_logDir = IniString(a_self, L"LogDir", g_logDir.c_str());
		CreateDirectoryW(g_logDir.c_str(), nullptr);
		g_logFile = _wfsopen((g_logDir + L"\\" + a_name + L".log").c_str(), L"w", _SH_DENYWR);
	}

	// ---- memory ---------------------------------------------------------------------------------
	inline bool SafeRead(void* a_dst, const void* a_src, size_t a_n)
	{
		__try {
			memcpy(a_dst, a_src, a_n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	inline bool SafeWrite(void* a_dst, const void* a_src, size_t a_n)
	{
		__try {
			memcpy(a_dst, a_src, a_n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	template <class T>
	inline bool Read(uintptr_t a_addr, T& a_out)
	{
		return a_addr && SafeRead(&a_out, reinterpret_cast<const void*>(a_addr), sizeof(T));
	}

	template <class T>
	inline T ReadOr(uintptr_t a_addr, T a_default)
	{
		T v;
		return Read(a_addr, v) ? v : a_default;
	}

	inline bool InModule(uintptr_t a_addr, uintptr_t a_base)
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(a_base);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
		return a_addr >= a_base && a_addr < a_base + nt->OptionalHeader.SizeOfImage;
	}

	inline void WriteCode(void* a_at, const void* a_bytes, size_t a_n)
	{
		DWORD old;
		VirtualProtect(a_at, a_n, PAGE_EXECUTE_READWRITE, &old);
		memcpy(a_at, a_bytes, a_n);
		VirtualProtect(a_at, a_n, old, &old);
		FlushInstructionCache(GetCurrentProcess(), a_at, a_n);
	}

	// ---- inline hook ----------------------------------------------------------------------------
	// Replaces `a_len` (>= 14) bytes of position-independent prologue, verified against `a_sig`,
	// with an absolute jump to `a_detour`. Returns the trampoline (call it as the original) or
	// nullptr when the bytes don't match (a different game build).
	inline void* InlineHook(uintptr_t a_target, const uint8_t* a_sig, size_t a_len, void* a_detour)
	{
		if (a_len < 14 || a_len > 32 || !InModule(a_target + a_len, g_exe) ||
			memcmp(reinterpret_cast<void*>(a_target), a_sig, a_len) != 0) {
			return nullptr;
		}
		auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!tramp) return nullptr;
		uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
		uintptr_t back = a_target + a_len;
		memcpy(tramp, a_sig, a_len);
		memcpy(jmp + 6, &back, 8);
		memcpy(tramp + a_len, jmp, 14);
		uint8_t patch[32];
		memset(patch, 0x90, sizeof(patch));
		uintptr_t to = reinterpret_cast<uintptr_t>(a_detour);
		memcpy(patch, jmp, 6);
		memcpy(patch + 6, &to, 8);
		WriteCode(reinterpret_cast<void*>(a_target), patch, a_len);
		return tramp;
	}

	// ---- IAT hook ---------------------------------------------------------------------------------
	// Points the main exe's import of a_dll!a_func at a_detour; returns the previous target.
	inline void* IatHook(const char* a_dll, const char* a_func, void* a_detour)
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
		auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) return nullptr;
		for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(g_exe + dir.VirtualAddress); imp->Name; ++imp) {
			if (_stricmp(reinterpret_cast<const char*>(g_exe + imp->Name), a_dll) != 0) continue;
			auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(g_exe + imp->OriginalFirstThunk);
			auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(g_exe + imp->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++iat) {
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
				auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(g_exe + names->u1.AddressOfData);
				if (strcmp(reinterpret_cast<const char*>(ibn->Name), a_func) != 0) continue;
				void* old = reinterpret_cast<void*>(iat->u1.Function);
				DWORD prot;
				VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &prot);
				iat->u1.Function = reinterpret_cast<ULONGLONG>(a_detour);
				VirtualProtect(&iat->u1.Function, 8, prot, &prot);
				return old;
			}
		}
		return nullptr;
	}

	// ---- vtable slot patch ------------------------------------------------------------------------
	// Points slot a_slot of a_obj's vtable at a_detour; returns the previous entry (nullptr if it was
	// already a_detour or the page can't be made writable). A vtable belongs to the class, so every
	// object of that class (in any module's hands) goes through the detour from then on.
	inline void* PatchVtableSlot(void* a_obj, int a_slot, void* a_detour)
	{
		void** vt = *reinterpret_cast<void***>(a_obj);
		if (vt[a_slot] == a_detour) return nullptr;
		void* orig = vt[a_slot];
		DWORD prot;
		if (!VirtualProtect(&vt[a_slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) return nullptr;
		vt[a_slot] = a_detour;
		VirtualProtect(&vt[a_slot], sizeof(void*), prot, &prot);
		return orig;
	}

	// ---- vtable "this" capture ------------------------------------------------------------------
	// Patches every slot of a vtable with a tiny thunk that stores `this` (rcx) into *a_store and
	// jumps to the original method. Arguments and return values pass through untouched.
	inline int CaptureThisOnVtable(uintptr_t a_vtable, int a_slots, void* volatile* a_store)
	{
		auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32 * a_slots, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!code) return 0;
		int patched = 0;
		for (int i = 0; i < a_slots; ++i) {
			auto*     slot = reinterpret_cast<uintptr_t*>(a_vtable + i * 8ull);
			uintptr_t orig = *slot;
			if (!InModule(orig, g_exe)) break;
			uint8_t* t = code + i * 32;
			// mov rax, imm64 (store) ; mov [rax], rcx ; mov rax, imm64 (orig) ; jmp rax
			t[0] = 0x48, t[1] = 0xB8;
			uintptr_t st = reinterpret_cast<uintptr_t>(a_store);
			memcpy(t + 2, &st, 8);
			t[10] = 0x48, t[11] = 0x89, t[12] = 0x08;
			t[13] = 0x48, t[14] = 0xB8;
			memcpy(t + 15, &orig, 8);
			t[23] = 0xFF, t[24] = 0xE0;
			uintptr_t to = reinterpret_cast<uintptr_t>(t);
			DWORD     prot;
			VirtualProtect(slot, 8, PAGE_READWRITE, &prot);
			*slot = to;
			VirtualProtect(slot, 8, prot, &prot);
			++patched;
		}
		FlushInstructionCache(GetCurrentProcess(), code, 32 * a_slots);
		return patched;
	}
}
