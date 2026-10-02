// ArkWeb recon for Batman: Arkham Knight (dinput8.dll proxy).
//
// Read-only: no hooks, no writes to game memory. It finds and dumps UE3's name and object tables
// and PhysX's root object on request through logs\ak_cmd.txt, so the SDK and unit scale can be
// worked out offline. Commands (plus the common ones in common/recon.h):
//   names [rva]                 GNames TArray (default exe+3a09f30) -> logs\ak_names.txt
//   gobjscan                    look for the GObjObjects TArray in the exe's data
//   objs <tarray> <hexbytes> <name>   every object's first <hexbytes> bytes -> logs\<name>.bin
//   physx                       PxGetPhysics(): pointer, vtable as PhysX3_x64.dll RVAs
//   objlist [hexbytes]          every object -> logs\ak_objects.txt; UField objects raw -> ak_fields.bin
//   inst <Class> <hexbytes> <file>   all instances of a class, raw -> logs\<file>.bin
//   find <Name>                 objects with that name
//   name <index>                FName lookup
#include "../common/fwd_dinput8.h"
#include "../common/recon.h"

using namespace recon;

namespace
{
	constexpr uintptr_t kGNamesCandidate = 0x3a09f30;

	struct TArrayRaw
	{
		uintptr_t data;
		int32_t   num;
		int32_t   max;
	};

	bool ExeSection(const char* a_name, uintptr_t& a_begin, size_t& a_size)
	{
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
		auto* sec = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			if (strncmp(reinterpret_cast<const char*>(sec[i].Name), a_name, 8) == 0) {
				a_begin = g_exe + sec[i].VirtualAddress;
				a_size = sec[i].Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	// Reads a NUL-terminated string at `a_p`: a_mode 1 ANSI, 2 UTF-16, 0 guess from the bytes.
	std::string ReadName(uintptr_t a_p, int a_mode = 0)
	{
		uint8_t b[2];
		if (!SafeRead(b, reinterpret_cast<void*>(a_p), 2)) return "<unreadable>";
		size_t n = 0;
		bool   wide = a_mode ? a_mode == 2 : (b[1] == 0 && b[0] != 0);
		std::string out;
		if (wide) {
			for (; n < 1000; ++n) {
				uint16_t c;
				if (!Read(a_p + n * 2, c) || c == 0) break;
				out.push_back(c < 128 ? static_cast<char>(c) : '?');
			}
		} else {
			for (; n < 1000; ++n) {
				char c;
				if (!Read(a_p + n, c) || c == 0) break;
				out.push_back(c);
			}
		}
		return out;
	}

	void DumpNames(uintptr_t a_rva)
	{
		TArrayRaw arr{};
		Read(g_exe + a_rva, arr);
		Log("GNames candidate exe+%llx: data=%p num=%d max=%d", static_cast<unsigned long long>(a_rva), reinterpret_cast<void*>(arr.data), arr.num, arr.max);
		HexDump(g_exe + a_rva - 0x10, 0x40);
		uintptr_t e0 = 0;
		if (!arr.data || !Read(arr.data, e0) || !e0) {
			Log("  entry 0 unreadable");
			return;
		}
		Log("  entry 0 at %p:", reinterpret_cast<void*>(e0));
		HexDump(e0, 0x40);
		int off = -1;
		uint8_t b[0x40];
		SafeRead(b, reinterpret_cast<void*>(e0), sizeof(b));
		for (int o = 0; o + 8 <= 0x40 && off < 0; ++o) {
			if (!memcmp(b + o, "None", 5) || !memcmp(b + o, "N\0o\0n\0e\0\0\0", 10)) off = o;
		}
		if (off < 0) {
			Log("  'None' not found in entry 0: wrong array");
			return;
		}
		Log("  name text at entry+0x%x", off);
		FILE* f = _wfopen(Path(L"ak_names.txt").c_str(), L"w");
		int   valid = 0;
		for (int i = 0; i < arr.num && i < 4000000; ++i) {
			uintptr_t e = 0;
			Read(arr.data + i * 8ull, e);
			if (!e) continue;
			std::string s = ReadName(e + off);
			fprintf(f, "%d\t%s\n", i, s.c_str());
			++valid;
		}
		fclose(f);
		Log("  wrote %d names to ak_names.txt", valid);
	}

	void GObjScan()
	{
		uintptr_t begin;
		size_t    size;
		if (!ExeSection(".data", begin, size)) return;
		Log("gobjscan over .data %p +%zx", reinterpret_cast<void*>(begin), size);
		int shown = 0;
		for (uintptr_t a = begin; a + 16 <= begin + size; a += 8) {
			TArrayRaw t;
			if (!Read(a, t) || t.num < 20000 || t.num > 4000000 || t.max < t.num || t.max > 8000000 || !Readable(t.data)) continue;
			// Most entries should be objects whose first qword is a vtable inside the exe.
			int good = 0, tested = 0;
			for (int s = 0; s < 64; ++s) {
				int       i = static_cast<int>((static_cast<int64_t>(t.num - 1) * s) / 63);
				uintptr_t obj = 0, vt = 0;
				if (!Read(t.data + i * 8ull, obj)) break;
				++tested;
				if (!obj) {
					++good;
					continue;
				}
				if (Read(obj, vt) && InModule(vt, g_exe)) ++good;
			}
			if (tested < 64 || good < 56) continue;
			// Which 4-byte field holds the object's own index?
			int bestOff = -1, bestHits = 0;
			for (int off = 8; off < 0x80; off += 4) {
				int hits = 0;
				for (int s = 0; s < 64; ++s) {
					int       i = static_cast<int>((static_cast<int64_t>(t.num - 1) * s) / 63);
					uintptr_t obj = 0;
					int32_t   v = -1;
					if (Read(t.data + i * 8ull, obj) && obj && Read(obj + off, v) && v == i) ++hits;
				}
				if (hits > bestHits) bestHits = hits, bestOff = off;
			}
			Log("  candidate exe+%llx: data=%p num=%d max=%d vtables %d/64, index field +0x%x (%d/64)",
				static_cast<unsigned long long>(a - g_exe), reinterpret_cast<void*>(t.data), t.num, t.max, good, bestOff, bestHits);
			if (++shown > 40) break;
		}
		Log("gobjscan done");
	}

	void DumpObjects(uintptr_t a_tarray, size_t a_bytes, const char* a_name)
	{
		TArrayRaw t{};
		Read(a_tarray, t);
		std::string n = a_name;
		FILE* f = _wfopen(Path(std::wstring(n.begin(), n.end()) + L".bin").c_str(), L"wb");
		if (!f) return;
		uint32_t hdr[2] = { static_cast<uint32_t>(t.num), static_cast<uint32_t>(a_bytes) };
		fwrite(hdr, 4, 2, f);
		std::vector<uint8_t> buf(a_bytes);
		int dumped = 0;
		for (int i = 0; i < t.num; ++i) {
			uintptr_t obj = 0;
			Read(t.data + i * 8ull, obj);
			std::fill(buf.begin(), buf.end(), 0);
			if (obj && SafeRead(buf.data(), reinterpret_cast<void*>(obj), a_bytes)) ++dumped;
			fwrite(&obj, 8, 1, f);
			fwrite(buf.data(), 1, a_bytes, f);
			if ((i & 0xFFF) == 0) Sleep(0);
		}
		fclose(f);
		Log("objs: %d of %d objects (%zx bytes each) -> %s.bin", dumped, t.num, a_bytes, a_name);
	}

	void PhysX()
	{
		HMODULE px = GetModuleHandleW(L"PhysX3_x64.dll");
		auto*   get = px ? reinterpret_cast<void* (*)()>(GetProcAddress(px, "PxGetPhysics")) : nullptr;
		if (!get) {
			Log("physx: PhysX3_x64.dll / PxGetPhysics not found");
			return;
		}
		auto      phys = reinterpret_cast<uintptr_t>(get());
		uintptr_t vt = 0;
		Read(phys, vt);
		Log("physx: dll %p, PxPhysics %p, vtable %p (PhysX3_x64+%llx)", px, reinterpret_cast<void*>(phys), reinterpret_cast<void*>(vt),
			static_cast<unsigned long long>(vt - reinterpret_cast<uintptr_t>(px)));
		for (int i = 0; i < 48; ++i) {
			uintptr_t fn = 0;
			if (!Read(vt + i * 8ull, fn)) break;
			Log("  slot %2d: PhysX3_x64+%llx", i, static_cast<unsigned long long>(fn - reinterpret_cast<uintptr_t>(px)));
		}
		HexDump(phys, 0x80);
	}

	// ---- UE3 object system (layouts found 2026-10-01; see recon/PHASE0b.md) ------------------------
	constexpr uintptr_t kGNames = 0x3a208b8;    // TArray<FNameEntry*>
	constexpr uintptr_t kGObjData = 0x340cbe4;  // UObject*[785000], inline (pack 4)
	constexpr uintptr_t kGObjNum = 0x3a09f28;   // int
	constexpr int kObjIndex = 0x30, kObjOuter = 0x34, kObjName = 0x3C, kObjClass = 0x44;

	std::string NameOf(int32_t a_index)
	{
		TArrayRaw arr{};
		Read(g_exe + kGNames, arr);
		uintptr_t e = 0;
		uint32_t  idx = 0;
		if (a_index < 0 || a_index >= arr.num || !Read(arr.data + a_index * 8ull, e) || !e || !Read(e, idx)) return "<bad name>";
		if (idx & 2) {  // out-of-line ANSI
			uintptr_t p = 0;
			Read(e + 0xC, p);
			return ReadName(p, 1);
		}
		return ReadName(e + 0xC, (idx & 1) ? 2 : 1);
	}

	int ObjCount()
	{
		int32_t n = 0;
		Read(g_exe + kGObjNum, n);
		return n < 0 ? 0 : n > 785000 ? 785000 : n;
	}

	uintptr_t ObjAt(int a_i)
	{
		uintptr_t o = 0;
		Read(g_exe + kGObjData + a_i * 8ull, o);
		return o;
	}

	std::string ObjName(uintptr_t a_obj)
	{
		int32_t n[2] = {};
		if (!a_obj || !Read(a_obj + kObjName, n)) return "<null>";
		std::string s = NameOf(n[0]);
		if (n[1] > 0) s += "_" + std::to_string(n[1] - 1);
		return s;
	}

	uintptr_t ObjPtr(uintptr_t a_obj, int a_off)
	{
		uintptr_t p = 0;
		Read(a_obj + a_off, p);
		return p;
	}

	std::string FullName(uintptr_t a_obj)
	{
		std::string s = ObjName(a_obj);
		for (uintptr_t o = ObjPtr(a_obj, kObjOuter), d = 0; o && d < 16; o = ObjPtr(o, kObjOuter), ++d) s = ObjName(o) + "." + s;
		return s;
	}

	int32_t ObjIdx(uintptr_t a_obj)
	{
		int32_t i = -1;
		if (a_obj) Read(a_obj + kObjIndex, i);
		return i;
	}

	// logs\ak_objects.txt: index, address, class, outer index, full name.
	// logs\ak_fields.bin: for each class/struct/function/state/enum/const/property object:
	//   u32 index, u32 bytes, then <bytes> raw bytes from the object.
	void ObjList(size_t a_fieldBytes)
	{
		FILE* t = _wfopen(Path(L"ak_objects.txt").c_str(), L"w");
		FILE* b = _wfopen(Path(L"ak_fields.bin").c_str(), L"wb");
		if (!t || !b) return;
		std::vector<uint8_t> buf(a_fieldBytes);
		int n = ObjCount(), live = 0, fields = 0;
		for (int i = 0; i < n; ++i) {
			uintptr_t o = ObjAt(i);
			if (!o) continue;
			++live;
			std::string cls = ObjName(ObjPtr(o, kObjClass));
			fprintf(t, "%d\t%p\t%s\t%d\t%s\n", i, reinterpret_cast<void*>(o), cls.c_str(), ObjIdx(ObjPtr(o, kObjOuter)), FullName(o).c_str());
			bool isField = cls == "Class" || cls == "ScriptStruct" || cls == "Function" || cls == "State" || cls == "Enum" ||
			               cls == "Const" || (cls.size() > 8 && cls.compare(cls.size() - 8, 8, "Property") == 0);
			if (isField) {
				std::fill(buf.begin(), buf.end(), 0);
				SafeRead(buf.data(), reinterpret_cast<void*>(o), a_fieldBytes);
				uint32_t hdr[2] = { static_cast<uint32_t>(i), static_cast<uint32_t>(a_fieldBytes) };
				fwrite(hdr, 4, 2, b);
				fwrite(buf.data(), 1, a_fieldBytes, b);
				++fields;
			}
			if ((i & 0x3FF) == 0) Sleep(0);
		}
		fclose(t);
		fclose(b);
		Log("objlist: %d objects (%d live), %d fields x %zx bytes", n, live, fields, a_fieldBytes);
	}

	// Every object whose class is named `a_class` (exact), first `a_bytes` bytes each -> <file>.bin
	// (u32 index, u64 address, bytes...), and their names in the log.
	void Instances(const char* a_class, size_t a_bytes, const char* a_file)
	{
		std::string n = a_file;
		FILE* f = _wfopen(Path(std::wstring(n.begin(), n.end()) + L".bin").c_str(), L"wb");
		if (!f) return;
		std::vector<uint8_t> buf(a_bytes);
		int count = 0;
		for (int i = 0, total = ObjCount(); i < total; ++i) {
			uintptr_t o = ObjAt(i);
			if (!o || ObjName(ObjPtr(o, kObjClass)) != a_class) continue;
			std::fill(buf.begin(), buf.end(), 0);
			SafeRead(buf.data(), reinterpret_cast<void*>(o), a_bytes);
			uint32_t idx = static_cast<uint32_t>(i);
			fwrite(&idx, 4, 1, f);
			fwrite(&o, 8, 1, f);
			fwrite(buf.data(), 1, a_bytes, f);
			if (count++ < 50) Log("  %d %p %s", i, reinterpret_cast<void*>(o), FullName(o).c_str());
		}
		fclose(f);
		Log("inst %s: %d objects -> %s.bin", a_class, count, a_file);
	}

	void Find(const char* a_name)
	{
		int count = 0;
		for (int i = 0, total = ObjCount(); i < total && count < 100; ++i) {
			uintptr_t o = ObjAt(i);
			if (o && ObjName(o) == a_name) {
				Log("  %d %p class=%s %s", i, reinterpret_cast<void*>(o), ObjName(ObjPtr(o, kObjClass)).c_str(), FullName(o).c_str());
				++count;
			}
		}
		Log("find %s: %d", a_name, count);
	}

	bool Command(char* a_line)
	{
		char* ctx = nullptr;
		char* cmd = strtok_s(a_line, " \t\r\n", &ctx);
		char* a1 = strtok_s(nullptr, " \t\r\n", &ctx);
		char* a2 = strtok_s(nullptr, " \t\r\n", &ctx);
		char* a3 = strtok_s(nullptr, " \t\r\n", &ctx);
		if (!_stricmp(cmd, "names")) {
			DumpNames(a1 ? strtoull(a1, nullptr, 16) : kGNamesCandidate);
		} else if (!_stricmp(cmd, "gobjscan")) {
			GObjScan();
		} else if (!_stricmp(cmd, "objs") && a3) {
			DumpObjects(ParseAddr(a1), strtoull(a2, nullptr, 16), a3);
		} else if (!_stricmp(cmd, "physx")) {
			PhysX();
		} else if (!_stricmp(cmd, "objlist")) {  // objlist [hexbytes per field object, default 140]
			ObjList(a1 ? strtoull(a1, nullptr, 16) : 0x140);
		} else if (!_stricmp(cmd, "inst") && a3) {  // inst <ClassName> <hexbytes> <file>
			Instances(a1, strtoull(a2, nullptr, 16), a3);
		} else if (!_stricmp(cmd, "find") && a1) {
			Find(a1);
		} else if (!_stricmp(cmd, "name") && a1) {
			Log("name %s = %s", a1, NameOf(atoi(a1)).c_str());
		} else {
			return false;
		}
		return true;
	}

	DWORD WINAPI Worker(void*)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
		for (int tick = 0;; ++tick) {
			Sleep(500);
			PollCommands(Command);
			if (tick % 120 == 0) Log("alive");  // every minute
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
		InitLog(a_self, L"ak");
		CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	}
	return TRUE;
}
