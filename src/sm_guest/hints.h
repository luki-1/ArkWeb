// Gotham swing hints.
//
// Spider-Man's SwingPointHunter does not find web anchors in the collision world. Every frame
// (exe+86f440) it queries a database of oriented boxes - the game's swing hint volumes, global at
// exe+5da0648 - around the hero (exe+1c897e0), turns up to 128 of them into records, puts candidate
// attach points on their faces and only then checks those with rays (ArkWeb/PHASE2.md). Gotham has
// no entries, so the hunter produces no candidates at all. We add one box per Gotham building part
// (build_gotham_hints.py) with the game's own insert (exe+1c88f30), and while the New York filter is
// on the query returns only our boxes.
//
// Database (SoA, capacity 8192): +0x00 spheres in blocks of four (x[4] y[4] z[4] r[4]), +0x08 record
// pointers, +0x10 u32 tags, +0x18 slot allocator, +0x38 highest slot. The query only uses spheres
// and returns record pointers.
// Record (0x70): +0x00/+0x10/+0x20 rotation rows (row1 = up), +0x30 position (w 1), +0x40 half
// extents, +0x4c center, +0x58 bounding radius (center + radius = the database sphere), +0x5c u32 1,
// +0x60 direction + flags byte +0x6c (pole / corner features; 0 for plain boxes).
#pragma once

#include "../common/util.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace arkweb::hints
{
	constexpr uintptr_t kDb = 0x5da0648;
	constexpr uintptr_t kInsert = 0x1c88f30;  // u16 Insert(db, const void* record, const float sphere[4], u32 tag); 0xffff = full
	constexpr uintptr_t kRemove = 0x1c89070;  // void Remove(db, u16 handle)
	constexpr uintptr_t kTagValue = 0x7a7fbf8;  // the tag SwingHintVolume passes (u32)
	constexpr uintptr_t kQuery = 0x1c897e0;   // int Query(db, const float sphere[4], const void** out, int max)
	constexpr uint8_t   kQuerySig[21] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x41,
		  0x55, 0x41, 0x57, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00 };
	constexpr size_t kRecordSize = 0x70;

	using InsertFn = uint32_t (*)(void* a_db, const void* a_record, const float* a_sphere, uint32_t a_tag);
	using RemoveFn = void (*)(void* a_db, uint32_t a_handle);
	using QueryFn = int (*)(void* a_db, const float* a_sphere, const void** a_out, int a_max);

	struct Hint
	{
		float rows[9], pos[3], half[3];
	};

	// Our records come from one pool (allocated once, never freed while the game runs), so "is this
	// one of ours" is a range check. Hints are kept in groups (a file, or one streamed tile) that are
	// inserted and removed as a whole.
	constexpr size_t kPool = 6000;  // the database holds 8192, New York uses about 2.5k

	struct Op
	{
		int               type;  // 1 insert group, 2 remove group, 3 remove all
		std::string       key;
		std::vector<Hint> hints;  // guest space, placement already applied
	};

	struct Placed
	{
		uint16_t slot;    // record index in the pool
		uint16_t handle;  // database handle
	};

	inline QueryFn                                    g_origQuery = nullptr;
	inline std::mutex                                 g_opsLock;
	inline std::vector<Op>                            g_ops;  // filled by the command thread, run by the hunter
	inline std::atomic<bool>                          g_haveOps{ false };
	inline uint8_t*                                   g_records = nullptr;
	inline std::vector<uint16_t>                      g_freeSlots;
	inline std::map<std::string, std::vector<Placed>> g_groups;  // hunter thread only
	inline std::atomic<int>                           g_inserted{ 0 };
	inline std::atomic<bool>                          g_onlyOurs{ false };  // New York filter on: queries see only Gotham hints
	inline std::atomic<bool>                          g_hideOurs{ false };  // gotham hide: queries never see Gotham hints
	inline std::atomic<uint64_t>                      g_queries{ 0 }, g_returnedOurs{ 0 }, g_droppedNy{ 0 };

	inline void* Db() { return reinterpret_cast<void*>(g_exe + kDb); }

	inline bool IsOurs(const void* a_p)
	{
		auto p = reinterpret_cast<const uint8_t*>(a_p);
		return g_records && p >= g_records && p < g_records + kPool * kRecordSize;
	}

	inline void Queue(Op&& a_op)
	{
		std::lock_guard<std::mutex> l(g_opsLock);
		g_ops.push_back(std::move(a_op));
		g_haveOps = true;
	}

	// "AWH1", u32 count, per hint 15 floats (rows, position, half extents); a_offset is added to the
	// positions (the file origin's placement). Queued as group a_key, replacing an older one.
	inline bool Load(const std::wstring& a_path, const double a_offset[3], const std::string& a_key = "file", bool a_quiet = false)
	{
		FILE* f = _wfopen(a_path.c_str(), L"rb");
		if (!f) {
			Log("hints: can't open %ls", a_path.c_str());
			return false;
		}
		char     magic[4];
		uint32_t n = 0;
		if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "AWH1", 4) || fread(&n, 4, 1, f) != 1 || n > 8192) {
			fclose(f);
			Log("hints: bad file %ls", a_path.c_str());
			return false;
		}
		std::vector<Hint> hints(n);
		size_t got = fread(hints.data(), sizeof(Hint), n, f);
		fclose(f);
		hints.resize(got);
		for (auto& h : hints) {
			for (int i = 0; i < 3; ++i) h.pos[i] = static_cast<float>(h.pos[i] + a_offset[i]);
		}
		if (!a_quiet) Log("hints: %zu loaded from %ls, inserted on the next hint query", hints.size(), a_path.c_str());
		Queue({ 1, a_key, std::move(hints) });
		return true;
	}

	// The same from AWH1 bytes (a tile record from the collision ring).
	inline bool LoadMem(const uint8_t* a_p, size_t a_n, const double a_offset[3], const std::string& a_key)
	{
		uint32_t n = 0;
		if (a_n < 8 || memcmp(a_p, "AWH1", 4)) return false;
		memcpy(&n, a_p + 4, 4);
		n = static_cast<uint32_t>(std::min<size_t>(n, (a_n - 8) / sizeof(Hint)));
		std::vector<Hint> hints(n);
		if (n) memcpy(hints.data(), a_p + 8, n * sizeof(Hint));
		for (auto& h : hints) {
			for (int i = 0; i < 3; ++i) h.pos[i] = static_cast<float>(h.pos[i] + a_offset[i]);
		}
		Queue({ 1, a_key, std::move(hints) });
		return true;
	}

	inline void RemoveGroup(const std::string& a_key)
	{
		auto it = g_groups.find(a_key);
		if (it == g_groups.end()) return;
		auto remove = reinterpret_cast<RemoveFn>(g_exe + kRemove);
		for (auto& p : it->second) {
			remove(Db(), p.handle);
			g_freeSlots.push_back(p.slot);
		}
		g_inserted -= static_cast<int>(it->second.size());
		g_groups.erase(it);
	}

	inline void InsertGroup(const std::string& a_key, const std::vector<Hint>& a_hints)
	{
		RemoveGroup(a_key);
		if (!g_records) {
			g_records = static_cast<uint8_t*>(_aligned_malloc(kPool * kRecordSize, 16));
			if (!g_records) return;
			for (size_t i = kPool; i-- > 0;) g_freeSlots.push_back(static_cast<uint16_t>(i));
		}
		uint32_t tag = ReadOr<uint32_t>(g_exe + kTagValue, 0);
		auto     insert = reinterpret_cast<InsertFn>(g_exe + kInsert);
		auto&    group = g_groups[a_key];
		int      full = 0;
		for (const Hint& h : a_hints) {
			if (g_freeSlots.empty()) {
				++full;
				continue;
			}
			uint16_t slot = g_freeSlots.back();
			auto*    r = reinterpret_cast<float*>(g_records + slot * kRecordSize);
			memset(r, 0, kRecordSize);
			for (int row = 0; row < 3; ++row) {
				for (int k = 0; k < 3; ++k) r[row * 4 + k] = h.rows[row * 3 + k];
			}
			r[12] = h.pos[0], r[13] = h.pos[1], r[14] = h.pos[2], r[15] = 1.0f;
			r[16] = h.half[0], r[17] = h.half[1], r[18] = h.half[2];
			r[19] = h.pos[0], r[20] = h.pos[1], r[21] = h.pos[2];
			r[22] = std::sqrt(h.half[0] * h.half[0] + h.half[1] * h.half[1] + h.half[2] * h.half[2]);
			*reinterpret_cast<uint32_t*>(r + 23) = 1;
			uint32_t handle = insert(Db(), r, r + 19, tag) & 0xffff;
			if (handle == 0xffff) {
				++full;
				continue;
			}
			g_freeSlots.pop_back();
			group.push_back({ slot, static_cast<uint16_t>(handle) });
		}
		g_inserted += static_cast<int>(group.size());
		if (full) Log("hints: group %s: %zu of %zu inserted, %d rejected (database or pool full; %d of ours in the database)", a_key.c_str(),
			group.size(), a_hints.size(), full, g_inserted.load());
	}

	inline void RunOps()
	{
		std::vector<Op> ops;
		{
			std::lock_guard<std::mutex> l(g_opsLock);
			ops.swap(g_ops);
			g_haveOps = false;
		}
		for (auto& op : ops) {
			if (op.type == 1) InsertGroup(op.key, op.hints);
			else if (op.type == 2) RemoveGroup(op.key);
			else if (op.type == 3) {
				while (!g_groups.empty()) RemoveGroup(g_groups.begin()->first);
				Log("hints: all removed");
			}
		}
	}

	// Inserts/removals happen here, on the hunter's own thread just before it reads the database
	// (exe+874c30 turned out not to be the per-frame path; that is exe+86f440, unhookable).
	inline int QueryDetour(void* a_db, const float* a_sphere, const void** a_out, int a_max)
	{
		if (a_db == Db() && g_haveOps.load(std::memory_order_relaxed)) RunOps();
		int n = g_origQuery(a_db, a_sphere, a_out, a_max);
		if (a_db != Db() || n <= 0 || !g_records) return n;
		g_queries.fetch_add(1, std::memory_order_relaxed);
		bool onlyOurs = g_onlyOurs.load(std::memory_order_relaxed), hideOurs = g_hideOurs.load(std::memory_order_relaxed);
		if (!onlyOurs && !hideOurs) return n;
		int kept = 0;
		for (int i = 0; i < n; ++i) {
			bool ours = IsOurs(a_out[i]);
			if (ours ? hideOurs : onlyOurs) {
				if (!ours) g_droppedNy.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			if (ours) g_returnedOurs.fetch_add(1, std::memory_order_relaxed);
			a_out[kept++] = a_out[i];
		}
		return kept;
	}

	inline void Install()
	{
		g_origQuery = reinterpret_cast<QueryFn>(InlineHook(g_exe + kQuery, kQuerySig, sizeof(kQuerySig), reinterpret_cast<void*>(&QueryDetour)));
		Log("swing hint hook: hint query %s", g_origQuery ? "ok" : "FAILED");
	}
}
