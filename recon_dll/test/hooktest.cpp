// Offline test of recon.h: inline hook under multithreaded calls, command file, qword scan.
#include "../common/recon.h"
#include <thread>
extern "C" uint64_t TargetFn(uint64_t, uint64_t);
using namespace recon;
static uint64_t (*g_orig)(uint64_t, uint64_t);
static std::atomic<uint64_t> g_hits{0};
static uint64_t Detour(uint64_t a, uint64_t b) { g_hits++; return g_orig(a, b) + 1000; }
static volatile uint64_t g_marker = 0x1122334455667788ull;
int main() {
	g_dir = L"H:/SteamLibrary/steamapps/common/Saints Row the Third/ArkWeb/recon_dll/test/logs";
	InitLog(GetModuleHandleW(nullptr), L"test");
	uint8_t sig[15] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18 };
	uint64_t before = TargetFn(2, 3);
	g_orig = reinterpret_cast<decltype(g_orig)>(Hook(reinterpret_cast<uintptr_t>(&TargetFn), sig, 15, reinterpret_cast<void*>(&Detour)));
	if (!g_orig) { printf("hook failed\n"); return 1; }
	uint64_t bad = 0;
	std::vector<std::thread> th;
	for (int t = 0; t < 4; ++t) th.emplace_back([&] { for (int i = 0; i < 200000; ++i) if (TargetFn(i, 1) != uint64_t(i) + 1 + 1000) bad++; });
	for (auto& t : th) t.join();
	printf("before=%llu after=%llu hits=%llu bad=%llu\n", before, TargetFn(2, 3), (unsigned long long)g_hits.load(), bad);
	FILE* f = _wfopen(Path(L"test_cmd.txt").c_str(), L"w");
	fprintf(f, "scan 1122334455667788 4\nptr %p\nmem %p 20\nbogus\n", (void*)&g_marker, (void*)&g_marker);
	fclose(f);
	PollCommands([](char*) { return false; });
	printf("marker at %p\n", (void*)&g_marker);
	return bad != 0;
}
