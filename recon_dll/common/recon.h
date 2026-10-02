// Shared pieces of the ArkWeb recon DLLs: logging to a fixed folder, a command file Claude writes
// while the game runs, crash-safe memory reads, memory scans and a minimal inline hook.
// Header-only; each DLL includes it once.
#pragma once

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace recon
{
	// Where logs, dumps and the command file live. An `arkweb_recon.ini` next to the DLL with a
	// single line `LogDir=<path>` overrides it.
	inline std::wstring g_dir = L"H:\\SteamLibrary\\steamapps\\common\\Saints Row the Third\\ArkWeb\\logs";
	inline std::wstring g_tag;  // "sm" / "ak": file name prefix
	inline FILE*        g_log = nullptr;
	inline SRWLOCK      g_logLock = SRWLOCK_INIT;
	inline uintptr_t    g_exe = 0;  // main module base

	inline std::wstring Path(const std::wstring& a_name) { return g_dir + L"\\" + a_name; }

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
		if (g_log) {
			fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
			fflush(g_log);
		}
		ReleaseSRWLockExclusive(&g_logLock);
	}

	inline void InitLog(HMODULE a_self, const wchar_t* a_tag)
	{
		g_tag = a_tag;
		g_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		wchar_t ini[MAX_PATH];
		GetModuleFileNameW(a_self, ini, MAX_PATH);
		if (wchar_t* slash = wcsrchr(ini, L'\\')) {
			wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"arkweb_recon.ini");
			if (FILE* f = _wfopen(ini, L"r, ccs=UTF-8")) {
				wchar_t line[MAX_PATH + 16];
				while (fgetws(line, MAX_PATH + 16, f)) {
					if (wcsncmp(line, L"LogDir=", 7) == 0) {
						std::wstring d = line + 7;
						while (!d.empty() && (d.back() == L'\n' || d.back() == L'\r' || d.back() == L' ')) d.pop_back();
						if (!d.empty()) g_dir = d;
					}
				}
				fclose(f);
			}
		}
		CreateDirectoryW(g_dir.c_str(), nullptr);
		g_log = _wfsopen(Path(g_tag + L"_recon.log").c_str(), L"w", _SH_DENYWR);
		Log("ArkWeb recon (%ls) loaded: exe base %p, pid %lu", a_tag, reinterpret_cast<void*>(g_exe), GetCurrentProcessId());
	}

	// ---- crash-safe memory access ------------------------------------------------------------
	inline bool SafeRead(void* a_dst, const void* a_src, size_t a_n)
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
		return SafeRead(&a_out, reinterpret_cast<const void*>(a_addr), sizeof(T));
	}

	inline bool Readable(uintptr_t a_addr)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (a_addr < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(a_addr), &mbi, sizeof(mbi))) return false;
		return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi.Protect != 0;
	}

	inline bool InModule(uintptr_t a_addr, uintptr_t a_base)
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(a_base);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
		return a_addr >= a_base && a_addr < a_base + nt->OptionalHeader.SizeOfImage;
	}

	inline void HexDump(uintptr_t a_addr, size_t a_n)
	{
		std::vector<uint8_t> b(a_n);
		if (!SafeRead(b.data(), reinterpret_cast<void*>(a_addr), a_n)) {
			Log("  %p: unreadable", reinterpret_cast<void*>(a_addr));
			return;
		}
		for (size_t o = 0; o < a_n; o += 16) {
			char line[160];
			int  p = snprintf(line, sizeof(line), "  +%04zx:", o);
			for (size_t i = 0; i < 16 && o + i < a_n; ++i) p += snprintf(line + p, sizeof(line) - p, " %02x", b[o + i]);
			p += snprintf(line + p, sizeof(line) - p, "  |");
			for (size_t i = 0; i < 16 && o + i < a_n; i += 4) {
				float f;
				memcpy(&f, &b[o + i], 4);
				p += snprintf(line + p, sizeof(line) - p, " %g", f);
			}
			Log("%s", line);
		}
	}

	inline bool DumpFile(const std::wstring& a_name, uintptr_t a_addr, size_t a_n)
	{
		std::vector<uint8_t> b(a_n);
		bool ok = SafeRead(b.data(), reinterpret_cast<void*>(a_addr), a_n);
		if (FILE* f = _wfopen(Path(a_name).c_str(), L"wb")) {
			fwrite(b.data(), 1, a_n, f);
			fclose(f);
		}
		return ok;
	}

	// Every committed, readable, writable private region (heaps and the like).
	template <class F>
	inline void ForEachHeapRegion(F&& a_fn)
	{
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t a = 0x10000;
		while (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
			uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
				(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD)) {
				a_fn(base, mbi.RegionSize);
			}
			a = base + mbi.RegionSize;
			if (a < base) break;
		}
	}

	inline size_t ScanRegion(uintptr_t a_base, size_t a_size, uint64_t a_value, uintptr_t* a_out, size_t a_cap)
	{
		size_t found = 0;
		__try {
			auto* p = reinterpret_cast<const uint64_t*>(a_base);
			for (size_t i = 0, n = a_size / 8; i < n && found < a_cap; ++i) {
				if (p[i] == a_value) a_out[found++] = a_base + i * 8;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return found;
	}

	// Addresses (8-aligned) holding the qword `a_value`.
	inline std::vector<uintptr_t> ScanQword(uint64_t a_value, size_t a_max)
	{
		std::vector<uintptr_t> out(a_max);
		size_t n = 0;
		ForEachHeapRegion([&](uintptr_t a_base, size_t a_size) {
			if (n < a_max) n += ScanRegion(a_base, a_size, a_value, out.data() + n, a_max - n);
			Sleep(0);
		});
		out.resize(n);
		return out;
	}

	// ---- minimal inline hook -------------------------------------------------------------------
	// Overwrites `a_len` (>= 14) bytes of position-independent prologue, verified against `a_sig`,
	// with an absolute jump. Returns the trampoline (original prologue + jump back) or nullptr.
	inline void* Hook(uintptr_t a_target, const uint8_t* a_sig, size_t a_len, void* a_detour)
	{
		if (a_len < 14 || !InModule(a_target + a_len, g_exe) || memcmp(reinterpret_cast<void*>(a_target), a_sig, a_len) != 0) return nullptr;
		auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!tramp) return nullptr;
		memcpy(tramp, a_sig, a_len);
		uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
		uintptr_t back = a_target + a_len;
		memcpy(jmp + 6, &back, 8);
		memcpy(tramp + a_len, jmp, 14);
		uint8_t patch[32];
		memset(patch, 0x90, sizeof(patch));
		uintptr_t to = reinterpret_cast<uintptr_t>(a_detour);
		memcpy(patch, jmp, 6);
		memcpy(patch + 6, &to, 8);
		DWORD old;
		VirtualProtect(reinterpret_cast<void*>(a_target), a_len, PAGE_EXECUTE_READWRITE, &old);
		memcpy(reinterpret_cast<void*>(a_target), patch, a_len);
		VirtualProtect(reinterpret_cast<void*>(a_target), a_len, old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(a_target), a_len);
		return tramp;
	}

	// ---- command file ----------------------------------------------------------------------------
	// Claude writes logs\<tag>_cmd.txt; the worker thread runs each line, logs the results and
	// deletes the file. Common commands:
	//   mem <addr> <size>            hex + float dump into the log   (addr: hex, or "exe+<rva>")
	//   bin <addr> <size> <name>     raw dump to logs\<name>.bin
	//   ptr <addr>                   read a qword
	//   scan <qword> [max]           heap addresses holding the qword (e.g. a vtable address)
	//   vt <rva> [max]               scan for objects whose vtable is exe+<rva>
	//   mod <dllname>                module base and size
	inline uintptr_t ParseAddr(const char* a_s)
	{
		if (!a_s) return 0;
		if (_strnicmp(a_s, "exe+", 4) == 0) return g_exe + strtoull(a_s + 4, nullptr, 16);
		return strtoull(a_s, nullptr, 16);
	}

	inline bool CommonCommand(char* a_line)
	{
		char* ctx = nullptr;
		char* cmd = strtok_s(a_line, " \t\r\n", &ctx);
		if (!cmd) return true;
		char* a1 = strtok_s(nullptr, " \t\r\n", &ctx);
		char* a2 = strtok_s(nullptr, " \t\r\n", &ctx);
		char* a3 = strtok_s(nullptr, " \t\r\n", &ctx);
		if (!_stricmp(cmd, "mem")) {
			uintptr_t a = ParseAddr(a1);
			size_t    n = a2 ? strtoull(a2, nullptr, 16) : 0x100;
			Log("mem %p +%zx", reinterpret_cast<void*>(a), n);
			HexDump(a, n > 0x4000 ? 0x4000 : n);
		} else if (!_stricmp(cmd, "bin") && a3) {
			uintptr_t a = ParseAddr(a1);
			size_t    n = strtoull(a2, nullptr, 16);
			std::string name = a3;
			bool ok = DumpFile(std::wstring(name.begin(), name.end()) + L".bin", a, n);
			Log("bin %p +%zx -> %s.bin %s", reinterpret_cast<void*>(a), n, a3, ok ? "ok" : "(partly unreadable: zeros)");
		} else if (!_stricmp(cmd, "ptr")) {
			uintptr_t a = ParseAddr(a1), v = 0;
			bool ok = Read(a, v);
			Log("ptr [%p] = %p%s (exe+%llx)", reinterpret_cast<void*>(a), reinterpret_cast<void*>(v), ok ? "" : " unreadable",
				static_cast<unsigned long long>(v - g_exe));
		} else if (!_stricmp(cmd, "scan") || !_stricmp(cmd, "vt")) {
			uint64_t v = !_stricmp(cmd, "vt") ? g_exe + strtoull(a1, nullptr, 16) : strtoull(a1, nullptr, 16);
			size_t   mx = a2 ? strtoull(a2, nullptr, 10) : 64;
			auto     hits = ScanQword(v, mx);
			Log("%s %llx: %zu hits", cmd, static_cast<unsigned long long>(v), hits.size());
			for (auto h : hits) Log("  %p", reinterpret_cast<void*>(h));
		} else if (!_stricmp(cmd, "mod")) {
			HMODULE m = GetModuleHandleA(a1);
			MODULEINFO mi{};
			if (m) GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
			Log("mod %s: base %p size %lx", a1 ? a1 : "?", m, mi.SizeOfImage);
		} else {
			return false;
		}
		return true;
	}

	// Runs `a_extra(line)` for commands CommonCommand doesn't know.
	template <class F>
	inline void PollCommands(F&& a_extra)
	{
		std::wstring file = Path(g_tag + L"_cmd.txt");
		FILE* f = _wfopen(file.c_str(), L"r");
		if (!f) return;
		std::vector<std::string> lines;
		char buf[1024];
		while (fgets(buf, sizeof(buf), f)) lines.emplace_back(buf);
		fclose(f);
		DeleteFileW(file.c_str());
		for (auto& l : lines) {
			if (l.empty() || l[0] == '#' || l[0] == '\n' || l[0] == '\r') continue;
			std::string copy = l;
			while (!copy.empty() && (copy.back() == '\n' || copy.back() == '\r')) copy.pop_back();
			Log("> %s", copy.c_str());
			std::vector<char> w(l.begin(), l.end());
			w.push_back(0);
			std::vector<char> w2 = w;
			if (!CommonCommand(w.data()) && !a_extra(w2.data())) Log("  unknown command");
		}
		Log("< done");
	}
}
