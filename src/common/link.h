// The shared-memory link both ArkWeb DLLs (and tests/link_test) use: mapping, seqlock slots and
// SPSC byte rings over the layout in protocol/arkweb_protocol.h.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../protocol/arkweb_protocol.h"

namespace arkweb
{
	class Link
	{
	public:
		// Creates the mapping or opens the existing one. Returns false if it can't be mapped or
		// holds an incompatible protocol.
		bool Open()
		{
			_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				static_cast<DWORD>(proto::kMappingBytes >> 32), static_cast<DWORD>(proto::kMappingBytes), proto::kMappingName);
			if (!_handle) return false;
			bool created = GetLastError() != ERROR_ALREADY_EXISTS;
			_base = static_cast<uint8_t*>(MapViewOfFile(_handle, FILE_MAP_ALL_ACCESS, 0, 0, proto::kMappingBytes));
			if (!_base) return false;
			auto* h = Header();
			if (created || h->magic == 0) {
				h->version = proto::kVersion;
				std::atomic_thread_fence(std::memory_order_release);
				h->magic = proto::kMagic;
			}
			return h->magic == proto::kMagic && h->version == proto::kVersion;
		}

		void Close()
		{
			if (_base) UnmapViewOfFile(_base);
			if (_handle) CloseHandle(_handle);
			_base = nullptr;
			_handle = nullptr;
		}

		bool Valid() const { return _base != nullptr; }
		uint8_t* Base() const { return _base; }

		proto::Header*     Header() const { return reinterpret_cast<proto::Header*>(_base + proto::kOffHeader); }
		proto::HostState*  Host() const { return reinterpret_cast<proto::HostState*>(_base + proto::kOffHostState); }
		proto::PadState*   Pad() const { return reinterpret_cast<proto::PadState*>(_base + proto::kOffPad); }
		proto::GuestState* Guest() const { return reinterpret_cast<proto::GuestState*>(_base + proto::kOffGuestState); }
		proto::CaptureState* Capture() const { return reinterpret_cast<proto::CaptureState*>(_base + proto::kOffCapture); }
		proto::PoseState*  Pose() const { return reinterpret_cast<proto::PoseState*>(_base + proto::kOffPose); }

		static bool Alive(uint64_t a_heartbeatMs)
		{
			return a_heartbeatMs && GetTickCount64() - a_heartbeatMs < proto::kHeartbeatTimeoutMs;
		}

	private:
		HANDLE   _handle = nullptr;
		uint8_t* _base = nullptr;
	};

	// ---- seqlock slots ---------------------------------------------------------------------------
	// Every slot struct starts with `uint32_t seq`. One writer per slot.
	template <class T>
	inline void SeqWrite(T* a_slot, const T& a_value)
	{
		auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(&a_slot->seq);
		uint32_t s = seq->load(std::memory_order_relaxed);
		seq->store(s + 1, std::memory_order_relaxed);  // odd: writing
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(reinterpret_cast<uint8_t*>(a_slot) + 4, reinterpret_cast<const uint8_t*>(&a_value) + 4, sizeof(T) - 4);
		std::atomic_thread_fence(std::memory_order_release);
		seq->store(s + 2, std::memory_order_release);
	}

	// False on a torn read (the writer was mid-update every try); keep the previous value then.
	template <class T>
	inline bool SeqRead(const T* a_slot, T& a_out, int a_tries = 8)
	{
		auto* seq = reinterpret_cast<const std::atomic<uint32_t>*>(&a_slot->seq);
		for (int i = 0; i < a_tries; ++i) {
			uint32_t s1 = seq->load(std::memory_order_acquire);
			if (s1 & 1) {
				YieldProcessor();
				continue;
			}
			std::memcpy(&a_out, a_slot, sizeof(T));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq->load(std::memory_order_relaxed) == s1) {
				a_out.seq = s1;
				return true;
			}
		}
		return false;
	}

	// ---- SPSC byte ring ----------------------------------------------------------------------------
	class Ring
	{
	public:
		Ring() = default;
		Ring(uint8_t* a_base, uint64_t a_totalBytes) :
			_head(reinterpret_cast<std::atomic<uint64_t>*>(a_base + proto::kRingHeadOff)),
			_tail(reinterpret_cast<std::atomic<uint64_t>*>(a_base + proto::kRingTailOff)),
			_data(a_base + proto::kRingDataOff),
			_cap(a_totalBytes - proto::kRingDataOff)
		{}

		static uint64_t Align8(uint64_t a_n) { return (a_n + 7) & ~7ull; }

		// Producer. False if there isn't room right now (caller retries later or drops).
		bool Write(uint32_t a_type, const void* a_payload, uint32_t a_bytes)
		{
			uint64_t need = Align8(sizeof(proto::RingRecord) + a_bytes);
			if (need > _cap / 2) return false;
			uint64_t head = _head->load(std::memory_order_relaxed);
			uint64_t tail = _tail->load(std::memory_order_acquire);
			uint64_t pos = head % _cap;
			uint64_t toEnd = _cap - pos;
			uint64_t pad = need > toEnd ? toEnd : 0;
			if (head + pad + need - tail > _cap) return false;
			if (pad) {
				if (pad >= sizeof(proto::RingRecord)) {
					proto::RingRecord r{ proto::kRecPad, static_cast<uint32_t>(pad - sizeof(proto::RingRecord)) };
					std::memcpy(_data + pos, &r, sizeof(r));
				}
				head += pad;
				pos = 0;
			}
			proto::RingRecord r{ a_type, a_bytes };
			std::memcpy(_data + pos, &r, sizeof(r));
			if (a_bytes) std::memcpy(_data + pos + sizeof(r), a_payload, a_bytes);
			_head->store(head + need, std::memory_order_release);
			return true;
		}

		// Consumer. Calls a_fn(type, payload, bytes) for every complete record; returns how many.
		template <class F>
		int Drain(F&& a_fn, int a_max = 1 << 30)
		{
			int      n = 0;
			uint64_t tail = _tail->load(std::memory_order_relaxed);
			uint64_t head = _head->load(std::memory_order_acquire);
			while (tail < head && n < a_max) {
				uint64_t pos = tail % _cap;
				uint64_t toEnd = _cap - pos;
				if (toEnd < sizeof(proto::RingRecord)) {  // writer skipped the sliver at the end
					tail += toEnd;
					continue;
				}
				proto::RingRecord r;
				std::memcpy(&r, _data + pos, sizeof(r));
				uint64_t size = Align8(sizeof(r) + r.bytes);
				if (r.type != proto::kRecPad) {
					a_fn(r.type, _data + pos + sizeof(r), r.bytes);
					++n;
				}
				tail += size;
				_tail->store(tail, std::memory_order_release);
			}
			return n;
		}

		uint64_t Pending() const { return _head->load(std::memory_order_acquire) - _tail->load(std::memory_order_acquire); }

		// Consumer: skip everything written so far (records from before this process attached).
		void Discard() { _tail->store(_head->load(std::memory_order_acquire), std::memory_order_release); }

	private:
		std::atomic<uint64_t>* _head = nullptr;
		std::atomic<uint64_t>* _tail = nullptr;
		uint8_t*               _data = nullptr;
		uint64_t               _cap = 0;
	};
}
