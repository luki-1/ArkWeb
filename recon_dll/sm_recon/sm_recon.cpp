// ArkWeb recon for Marvel's Spider-Man Remastered v4.0630 (winmm.dll proxy).
//
// Observes only. It hooks the three hknpWorld query entry points to count calls, attribute
// callers and sample raw query structs; it never changes a result. Everything else is reading
// memory on request through logs\sm_cmd.txt (see common/recon.h and the commands below).
#include "../common/fwd_winmm.h"
#include "../common/recon.h"

using namespace recon;

namespace
{
	// ---- targets (RVAs, Spider-Man.exe 4.0630.0.0; see recon/PHASE0.md) ----------------------------
	constexpr uintptr_t kWorldGlobal = 0x78939e8;  // hknpWorld* of the game world
	constexpr const char* kQueryName[3] = { "castRay", "castShape", "getClosestPoints" };
	constexpr uintptr_t kQueryFn[3] = { 0x2e67010, 0x2e670f0, 0x2e671f0 };
	constexpr uint8_t   kPrologue[15] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };

	struct VtName
	{
		uintptr_t   rva;
		const char* name;
	};
	constexpr VtName kHeroVtables[] = {
		{ 0x38a93c8, "Hero::HeroLocal" },
		{ 0x38b1dd0, "Hero::HeroCameraManager" },
		{ 0x38b2c98, "Hero::HeroMoverManager" },
		{ 0x38ce990, "Hero::HeroStateSwingLocal" },
		{ 0x38aacb0, "Hero::HeroIKManager" },
		{ 0x38b2148, "Hero::HeroCharacterManager" },
		{ 0x38b1658, "Hero::HeroAimManagerLocal" },
		{ 0x38b3df8, "Hero::HeroRopeManager" },
	};

	// ---- stats (written from any thread, lock-free) -------------------------------------------------
	using QueryFn = void* (*)(void*, void*, void*, void*, void*, void*);
	QueryFn g_orig[3] = {};
	std::atomic<uint64_t> g_calls[3];
	std::atomic<uint64_t> g_mainWorld[3];

	constexpr size_t kCallers = 1024;
	struct Caller
	{
		std::atomic<uintptr_t> ra;
		std::atomic<uint32_t>  count[3];
	};
	Caller g_callers[kCallers];

	void CountCaller(int a_k, uintptr_t a_ra)
	{
		size_t h = (a_ra >> 4) * 0x9E3779B97F4A7C15ull >> 54;
		for (size_t i = 0; i < 32; ++i) {
			Caller& c = g_callers[(h + i) & (kCallers - 1)];
			uintptr_t cur = c.ra.load(std::memory_order_relaxed);
			if (cur == 0 && c.ra.compare_exchange_strong(cur, a_ra)) cur = a_ra;  // claimed the slot (or cur = winner)
			if (cur == a_ra) {
				c.count[a_k].fetch_add(1, std::memory_order_relaxed);
				return;
			}
		}
	}

	// Raw query samples, filled by the hooks and logged by the worker.
	struct Sample
	{
		int       k;
		uintptr_t world;
		uintptr_t args[4];
		uint8_t   query[0xA0];
		void*     stack[10];
		USHORT    frames;
		DWORD     thread;
		std::atomic<int> ready;
	};
	constexpr int     kSamples = 64;
	Sample            g_samples[kSamples];
	std::atomic<int>  g_sampleLeft{ 0 };
	std::atomic<int>  g_sampleNext{ 0 };
	std::atomic<int>  g_sampleKind{ -1 };  // -1: any

	template <int K>
	void* Detour(void* a_world, void* a_query, void* a_c, void* a_d, void* a_e, void* a_f)
	{
		g_calls[K].fetch_add(1, std::memory_order_relaxed);
		uintptr_t main = 0;
		Read(g_exe + kWorldGlobal, main);
		if (reinterpret_cast<uintptr_t>(a_world) == main) g_mainWorld[K].fetch_add(1, std::memory_order_relaxed);
		CountCaller(K, reinterpret_cast<uintptr_t>(_ReturnAddress()));

		int kind = g_sampleKind.load(std::memory_order_relaxed);
		if ((kind < 0 || kind == K) && g_sampleLeft.load(std::memory_order_relaxed) > 0 && g_sampleLeft.fetch_sub(1) > 0) {
			Sample& s = g_samples[g_sampleNext.fetch_add(1) % kSamples];
			if (s.ready.load() == 0) {
				s.k = K;
				s.world = reinterpret_cast<uintptr_t>(a_world);
				s.args[0] = reinterpret_cast<uintptr_t>(a_query);
				s.args[1] = reinterpret_cast<uintptr_t>(a_c);
				s.args[2] = reinterpret_cast<uintptr_t>(a_d);
				s.args[3] = reinterpret_cast<uintptr_t>(a_e);
				if (!SafeRead(s.query, a_query, sizeof(s.query))) memset(s.query, 0xEE, sizeof(s.query));
				s.frames = RtlCaptureStackBackTrace(1, 10, s.stack, nullptr);
				s.thread = GetCurrentThreadId();
				s.ready.store(1);
			}
		}
		return g_orig[K](a_world, a_query, a_c, a_d, a_e, a_f);
	}

	void* const kDetours[3] = { reinterpret_cast<void*>(&Detour<0>), reinterpret_cast<void*>(&Detour<1>), reinterpret_cast<void*>(&Detour<2>) };

	void InstallHooks()
	{
		for (int k = 0; k < 3; ++k) {
			void* t = Hook(g_exe + kQueryFn[k], kPrologue, sizeof(kPrologue), kDetours[k]);
			g_orig[k] = reinterpret_cast<QueryFn>(t);
			Log("hook hknpWorld::%s @ exe+%llx: %s", kQueryName[k], static_cast<unsigned long long>(kQueryFn[k]),
				t ? "ok" : "SKIPPED (prologue differs: wrong game version?)");
		}
	}

	std::string Rva(uintptr_t a_addr)
	{
		char buf[64];
		if (a_addr >= g_exe && InModule(a_addr, g_exe)) {
			snprintf(buf, sizeof(buf), "exe+%llx", static_cast<unsigned long long>(a_addr - g_exe));
		} else {
			HMODULE m = nullptr;
			char    name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(a_addr), &m)) {
				GetModuleFileNameA(m, name, MAX_PATH);
				const char* b = strrchr(name, '\\');
				snprintf(buf, sizeof(buf), "%s+%llx", b ? b + 1 : name, static_cast<unsigned long long>(a_addr - reinterpret_cast<uintptr_t>(m)));
			} else {
				snprintf(buf, sizeof(buf), "%p", reinterpret_cast<void*>(a_addr));
			}
		}
		return buf;
	}

	void LogSamples()
	{
		uintptr_t main = 0;
		Read(g_exe + kWorldGlobal, main);
		for (auto& s : g_samples) {
			if (s.ready.load() != 1) continue;
			Log("sample %s world=%p%s query=%p c=%p d=%p e=%p thread=%lu", kQueryName[s.k], reinterpret_cast<void*>(s.world),
				s.world == main ? " (main)" : "",
				reinterpret_cast<void*>(s.args[0]), reinterpret_cast<void*>(s.args[1]), reinterpret_cast<void*>(s.args[2]),
				reinterpret_cast<void*>(s.args[3]), s.thread);
			std::string st;
			for (USHORT i = 0; i < s.frames; ++i) st += " " + Rva(reinterpret_cast<uintptr_t>(s.stack[i]));
			Log("  stack:%s", st.c_str());
			for (size_t o = 0; o < sizeof(s.query); o += 16) {
				float f[4];
				memcpy(f, s.query + o, 16);
				const uint8_t* b = s.query + o;
				Log("  +%02zx: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x | %g %g %g %g", o,
					b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], f[0], f[1], f[2], f[3]);
			}
			s.ready.store(0);
		}
	}

	uint64_t g_lastCalls[3] = {};
	uint64_t g_lastMain[3] = {};
	ULONGLONG g_lastTick = 0;

	void LogRates()
	{
		ULONGLONG now = GetTickCount64();
		double    dt = g_lastTick ? (now - g_lastTick) / 1000.0 : 0.0;
		g_lastTick = now;
		uintptr_t main = 0;
		Read(g_exe + kWorldGlobal, main);
		char line[512];
		int  p = snprintf(line, sizeof(line), "rates: world=%p", reinterpret_cast<void*>(main));
		for (int k = 0; k < 3; ++k) {
			uint64_t c = g_calls[k].load(), m = g_mainWorld[k].load();
			if (dt > 0) p += snprintf(line + p, sizeof(line) - p, "  %s %.0f/s (main %.0f/s)", kQueryName[k], (c - g_lastCalls[k]) / dt, (m - g_lastMain[k]) / dt);
			g_lastCalls[k] = c;
			g_lastMain[k] = m;
		}
		if (dt > 0) Log("%s", line);
	}

	void LogCallers()
	{
		struct Row
		{
			uintptr_t ra;
			uint32_t  n[3];
		};
		std::vector<Row> rows;
		for (auto& c : g_callers) {
			if (uintptr_t ra = c.ra.load()) rows.push_back({ ra, { c.count[0].load(), c.count[1].load(), c.count[2].load() } });
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.n[0] + a.n[1] + a.n[2] > b.n[0] + b.n[1] + b.n[2]; });
		Log("callers (return address: ray / shape / closest), %zu distinct", rows.size());
		for (auto& r : rows) Log("  %-28s %10u %10u %10u", Rva(r.ra).c_str(), r.n[0], r.n[1], r.n[2]);
	}

	bool Command(char* a_line)
	{
		char* ctx = nullptr;
		char* cmd = strtok_s(a_line, " \t\r\n", &ctx);
		char* a1 = strtok_s(nullptr, " \t\r\n", &ctx);
		char* a2 = strtok_s(nullptr, " \t\r\n", &ctx);
		if (!_stricmp(cmd, "callers")) {
			LogCallers();
		} else if (!_stricmp(cmd, "sample")) {  // sample <n> [ray|shape|closest]
			int kind = -1;
			if (a2) kind = !_stricmp(a2, "ray") ? 0 : !_stricmp(a2, "shape") ? 1 : 2;
			g_sampleKind = kind;
			g_sampleLeft = a1 ? atoi(a1) : 8;
		} else if (!_stricmp(cmd, "hero")) {  // every object whose vtable is one of the hero classes
			for (auto& v : kHeroVtables) {
				auto hits = ScanQword(g_exe + v.rva, 16);
				Log("%s (vtable exe+%llx): %zu", v.name, static_cast<unsigned long long>(v.rva), hits.size());
				for (auto h : hits) Log("  %p", reinterpret_cast<void*>(h));
			}
		} else if (!_stricmp(cmd, "world")) {
			uintptr_t main = 0;
			Read(g_exe + kWorldGlobal, main);
			Log("hknpWorld* [exe+%llx] = %p", static_cast<unsigned long long>(kWorldGlobal), reinterpret_cast<void*>(main));
			if (main) HexDump(main, 0x100);
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
			LogSamples();
			if (tick % 20 == 0) LogRates();  // every 10 s
		}
	}
}

BOOL APIENTRY DllMain(HMODULE a_self, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(a_self);
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		if (!wcsstr(exe, L"Spider-Man.exe")) return TRUE;  // some other process picked up the proxy
		InitLog(a_self, L"sm");
		InstallHooks();
		CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
	}
	return TRUE;
}
