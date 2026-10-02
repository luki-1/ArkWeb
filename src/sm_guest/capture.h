// Phase V: Spider-Man's own picture of the hero, for Arkham to draw over its view.
//
// In sky mode the hero swings a kilometer above New York, so whatever is near Spider-Man's camera is
// the hero and his webs. Every presented frame is copied together with the depth buffer, and a small
// compute pass keeps only the pixels nearer than `range`: (r, g, b, distance), cropped to the 3D view
// (the game letterboxes it in a window of another shape). The host opens the results and draws them
// over its own frame (protocol CaptureState).
//
// Sharing: by default the frames go through D3D11 textures with legacy shared handles, made on a
// D3D11On12 device on the game's own queue (what OBS does for D3D12 games); D3D12 resources shared by
// name (`cap share named`) failed to open in Arkham's D3D11 with E_INVALIDARG.
//
// The hooks are vtable slots of the real D3D12/DXGI classes. A vtable belongs to a class, so every
// instance goes through it - also the objects NVIDIA Streamline wraps, whose calls end in the real
// ones: command list ResourceBarrier / OMSetRenderTargets / ClearDepthStencilView / BeginRenderPass,
// command queue ExecuteCommandLists, device CreateDepthStencilView (patched when the game creates its
// device, before it creates anything else), swap chain Present / Present1 / ResizeBuffers (patched
// from a throwaway swap chain once the game window exists).
//
// Facts from the first run (2026-10-01): the scene depth is R32G8X24 (D32S8) at the render
// resolution, reversed Z (cleared to 0), near plane 0.1 m (the follow camera's +0x7C); after the
// G-buffer pass it goes DEPTH_WRITE -> GENERIC_READ and back to DEPTH_WRITE for a forward pass, and
// stays DEPTH_WRITE to the end of the frame. In the sky everything within 10 m is the hero and nothing
// else is nearer than 2 km.
#pragma once

#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "../common/link.h"
#include "../common/util.h"
#include "capture_cs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace arkweb::capture
{
	// vtable slots (Windows SDK 10.0.19041 C vtables of d3d12.h / dxgi1_4.h)
	constexpr int kDevCreateDsv = 21;
	constexpr int kQueueExecute = 10;
	constexpr int kListBarrier = 26, kListSetRts = 46, kListClearDsv = 47, kListBeginRenderPass = 68;
	constexpr int kSwapPresent = 8, kSwapResizeBuffers = 13, kSwapPresent1 = 22;

	// Spider-Man's gameplay camera: Camera2::FollowCamera (RTTI vtable), its field of view in radians at
	// +0x74 (GetCameraFOVAction reads it there), near / far plane at +0x7C / +0x80.
	constexpr uintptr_t kFollowCameraVtable = 0x38715c8;
	constexpr int       kCamFov = 0x74, kCamNear = 0x7c, kCamFar = 0x80;

	using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
	using CreateDsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
	using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
	using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
	using SetRtsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
	using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
	using BeginPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
		const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
	using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	using SerializeRootSigFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

	inline CreateDeviceFn g_realCreateDevice = nullptr;  // what the game got from GetProcAddress
	inline CreateDsvFn    g_origCreateDsv = nullptr;
	inline ExecuteFn      g_origExecute = nullptr;
	inline BarrierFn      g_origBarrier = nullptr;
	inline SetRtsFn       g_origSetRts = nullptr;
	inline ClearDsvFn     g_origClearDsv = nullptr;
	inline BeginPassFn    g_origBeginPass = nullptr;
	inline PresentFn      g_origPresent = nullptr;
	inline Present1Fn     g_origPresent1 = nullptr;
	inline ResizeFn       g_origResize = nullptr;

	// ---- settings (`cap ...` commands) ------------------------------------------------------------
	inline std::atomic<int>  g_mode{ 2 };             // 0 off, 1 on, 2 auto (on while main.cpp sets g_autoWant)
	inline std::atomic<bool> g_autoWant{ false };     // the hero is on Gotham in the sky and Arkham is linked
	inline std::atomic<int>  g_probeFrames{ 0 };      // frames left to log
	inline std::atomic<bool> g_dumpRequest{ false };
	inline float             g_near = 0.1f;           // projection near plane (m); the follow camera's when it is found
	inline bool              g_reversed = true;       // reversed Z
	inline float             g_range = 150.0f;        // keep what is nearer (m)
	inline float             g_fovYDeg = 0.0f;        // `cap fov`: vertical FOV when the follow camera isn't found (0 = unknown)
	inline bool              g_fovHorizontal = true;  // the follow camera's FOV is horizontal (`cap fovmode h|v`)
	inline uintptr_t         g_depthPick = 0;         // `cap depth <addr>`: that resource; 0 = choose automatically
	inline int               g_point = 0;             // depth copy: 0 at Present (whole frame), 1 after the first pass's barrier
	inline proto::CaptureShare g_share = proto::kShareLegacy;

	// ---- set by main.cpp -----------------------------------------------------------------------------
	inline proto::CaptureState* g_state = nullptr;           // the link's capture slot
	inline std::atomic<bool>    g_windowReady{ false };      // the game window exists
	inline bool (*g_readCamera)(double a_pos[3], float a_rot[9]) = nullptr;  // camera, guest space (anchored)
	inline void (*g_dumpExtra)(FILE* a_f) = nullptr;         // more to write into a dump's meta file

	inline bool Enabled()
	{
		int m = g_mode.load(std::memory_order_relaxed);
		return m == 1 || (m == 2 && g_autoWant.load(std::memory_order_relaxed));
	}

	// ---- state ---------------------------------------------------------------------------------------
	inline std::atomic<bool>                 g_hooked{ false }, g_swapHooked{ false };
	inline thread_local bool                 t_ours = false;  // our own submissions pass the hooks untouched
	inline std::atomic<ID3D12CommandQueue*>  g_presentQueue{ nullptr };
	inline std::atomic<uint64_t>             g_presents{ 0 };
	inline std::atomic<uint32_t>             g_copiesThisFrame{ 0 }, g_copiesLastFrame{ 0 };
	inline std::atomic<uint64_t>             g_framesDone{ 0 }, g_framesSkipped{ 0 }, g_occluded{ 0 };

	inline SRWLOCK                                     g_dsvLock = SRWLOCK_INIT;
	inline std::unordered_map<SIZE_T, ID3D12Resource*> g_dsvs;  // DSV descriptor -> resource (never dereferenced)

	// depth buffers seen in barriers (desc taken while the game was using them)
	struct DepthInfo
	{
		ID3D12Resource*     res;
		D3D12_RESOURCE_DESC desc;
		uint64_t            lastFrame;
		uint32_t            ends;      // DEPTH_WRITE -> something else, this frame
		uint32_t            endsLast;  // ... last frame
	};
	inline SRWLOCK   g_candLock = SRWLOCK_INIT;
	inline DepthInfo g_cands[16];
	inline int       g_candCount = 0;

	// the depth buffer being copied and where to (replaced as a whole; old ones linger for 300 frames)
	struct DepthTarget
	{
		ID3D12Resource*                    game;
		D3D12_RESOURCE_DESC                desc;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
		UINT64                             bytes;
		ID3D12Resource*                    buf;  // DEFAULT heap buffer, COMMON (copies promote it)
	};
	inline std::atomic<DepthTarget*>          g_depthTarget{ nullptr };
	inline std::atomic<D3D12_RESOURCE_STATES> g_depthState{ D3D12_RESOURCE_STATE_DEPTH_WRITE };  // after its last barrier
	struct Grave
	{
		DepthTarget* t;
		uint64_t     frame;
	};
	inline std::vector<Grave> g_graveyard;

	// present-side resources
	struct FrameSlot
	{
		ID3D12CommandAllocator* alloc = nullptr;
		UINT64                  fence = 0;
	};
	inline ID3D12Device*              g_dev = nullptr;
	inline ID3D12GraphicsCommandList* g_list = nullptr;
	inline FrameSlot                  g_frames[3];
	inline std::atomic<ID3D12Fence*>  g_fence{ nullptr };
	inline HANDLE                     g_fenceEvent = nullptr;
	inline UINT64                     g_fenceValue = 0;
	inline ID3D12RootSignature*       g_rootSig = nullptr;
	inline ID3D12PipelineState*       g_pso = nullptr;
	inline ID3D12DescriptorHeap*      g_heap = nullptr;
	inline UINT                       g_descSize = 0;
	inline bool                       g_staticFailed = false;
	inline ID3D12Resource*            g_privColor = nullptr;
	inline DXGI_FORMAT                g_colorFmt = DXGI_FORMAT_UNKNOWN;
	inline UINT                       g_bbW = 0, g_bbH = 0;                    // back buffer
	inline UINT                       g_w = 0, g_h = 0, g_x0 = 0, g_y0 = 0;    // the 3D view inside it = output size
	inline UINT                       g_gen = 0, g_slot = 0;
	inline proto::CaptureShare        g_madeShare = proto::kShareLegacy;      // how the current outputs are shared
	inline ID3D12Resource*            g_outTex[proto::kCaptureSlots] = {};     // compute target (UAV)
	inline HANDLE                     g_outHandle[proto::kCaptureSlots] = {};  // kShareNamed: NT handles holding the names
	inline DepthTarget*               g_boundDepth = nullptr;

	// D3D11On12 (kShareLegacy): shared D3D11 textures, filled from the wrapped outputs
	inline ID3D11Device*        g_dev11 = nullptr;
	inline ID3D11DeviceContext* g_ctx11 = nullptr;
	inline ID3D11On12Device*    g_11on12 = nullptr;
	inline bool                 g_11on12Failed = false;
	inline ID3D11Texture2D*     g_shared11[proto::kCaptureSlots] = {};
	inline ID3D11Resource*      g_wrapped[proto::kCaptureSlots] = {};
	inline HANDLE               g_legacy[proto::kCaptureSlots] = {};

	// frames waiting for the GPU, published by the capture thread
	struct Pending
	{
		UINT64  fence;
		UINT    slot, gen;
		int64_t qpc;
		double  camPos[3];
		float   camRot[9];
		float   tanX, tanY;
	};
	inline SRWLOCK   g_pendLock = SRWLOCK_INIT;
	inline Pending   g_pend[8];
	inline UINT64    g_publishedFence = 0;
	inline ULONGLONG g_lastPublishMs = 0;

	// one-shot dump of colour + depth
	inline ID3D12Resource*                    g_rbColor = nullptr;
	inline ID3D12Resource*                    g_rbDepth = nullptr;
	inline D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_rbColorFp{};
	inline UINT64                             g_rbColorBytes = 0, g_rbDepthBytes = 0;
	inline DepthTarget                        g_rbDepthOf{};
	inline std::atomic<UINT64>                g_dumpFence{ 0 };
	inline float                              g_dumpTan[2] = {}, g_dumpFov = 0.0f;
	inline UINT                               g_dumpRect[4] = {};

	// the follow camera (found by its vtable; capture thread)
	inline std::atomic<uintptr_t> g_followCam{ 0 };
	inline ULONGLONG              g_lastCamScan = 0;
	inline std::atomic<bool>      g_camScanWanted{ false }, g_camScanning{ false };

	// ---- helpers ---------------------------------------------------------------------------------------
	inline std::string ModuleOf(const void* a_addr)
	{
		HMODULE m = nullptr;
		char    name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(a_addr), &m))
			GetModuleFileNameA(m, name, MAX_PATH);
		const char* s = strrchr(name, '\\');
		return s ? s + 1 : name;
	}

	inline bool IsDepthFamily(DXGI_FORMAT a_f)
	{
		return a_f == DXGI_FORMAT_R32_TYPELESS || a_f == DXGI_FORMAT_D32_FLOAT || a_f == DXGI_FORMAT_R32G8X24_TYPELESS ||
			a_f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	}

	inline std::atomic<int> g_probeLines{ 0 };
	inline bool             Probing() { return g_probeFrames.load(std::memory_order_relaxed) > 0; }

	template <class... A>
	inline void ProbeLog(const char* a_fmt, A... a_args)
	{
		if (g_probeLines.fetch_add(1, std::memory_order_relaxed) < 400) Log(a_fmt, a_args...);
	}

	inline ID3D12Resource* DsvResource(SIZE_T a_handle)
	{
		AcquireSRWLockShared(&g_dsvLock);
		auto            it = g_dsvs.find(a_handle);
		ID3D12Resource* r = it != g_dsvs.end() ? it->second : nullptr;
		ReleaseSRWLockShared(&g_dsvLock);
		return r;
	}

	// ---- the follow camera's lens ----------------------------------------------------------------------
	inline bool ValidCamera(uintptr_t a_cam)
	{
		float fov = 0, nearZ = 0, farZ = 0;
		return a_cam && ReadOr<uintptr_t>(a_cam, 0) == g_exe + kFollowCameraVtable && Read(a_cam + kCamFov, fov) && Read(a_cam + kCamNear, nearZ) &&
			Read(a_cam + kCamFar, farZ) && fov > 0.2f && fov < 3.0f && nearZ > 0.001f && nearZ < 2.0f && farZ > 10.0f;
	}

	// Field of view (radians, as the camera stores it) and near plane; false when the camera isn't known.
	inline bool CameraLens(float& a_fov, float& a_near)
	{
		uintptr_t cam = g_followCam.load(std::memory_order_relaxed);
		if (!ValidCamera(cam)) {
			if (cam) g_followCam = 0;
			g_camScanWanted = true;
			return false;
		}
		return Read(cam + kCamFov, a_fov) && Read(cam + kCamNear, a_near);
	}

	// tan of the half field of view across and up, for a view of aspect a_aspect.
	inline bool LensTan(float a_aspect, float& a_tx, float& a_ty)
	{
		float fov, nearZ;
		if (CameraLens(fov, nearZ)) {
			float t = std::tan(fov * 0.5f);
			a_tx = g_fovHorizontal ? t : t * a_aspect;
			a_ty = g_fovHorizontal ? t / a_aspect : t;
			return true;
		}
		if (g_fovYDeg > 1.0f) {
			a_ty = std::tan(g_fovYDeg * 0.5f * 3.14159265f / 180.0f);
			a_tx = a_ty * a_aspect;
			return true;
		}
		a_tx = a_ty = 0.0f;
		return false;
	}

	// The 3D view's aspect (the depth buffer's), else 16:10.
	inline float ViewAspect()
	{
		DepthTarget* dt = g_depthTarget.load();
		return dt && dt->desc.Height ? static_cast<float>(dt->desc.Width) / dt->desc.Height : 1.6f;
	}

	// Vertical FOV in degrees for GuestState (0 = not known).
	inline float FovYDeg()
	{
		float tx, ty;
		return LensTan(ViewAspect(), tx, ty) ? 2.0f * std::atan(ty) * 180.0f / 3.14159265f : 0.0f;
	}

	// The game's FollowCamera objects, by vtable, in its private memory (capture thread; ~1 s, idle).
	inline void FindCamera()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
		const uintptr_t          want = g_exe + kFollowCameraVtable;
		MEMORY_BASIC_INFORMATION mbi;
		std::vector<uint8_t>     buf(1 << 20);
		int                      found = 0;
		uintptr_t                first = 0;
		ULONGLONG                t0 = GetTickCount64();
		for (uint8_t* a = nullptr; VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi); a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
			if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) ||
				!(mbi.Protect & PAGE_READWRITE) || mbi.RegionSize > (256u << 20))
				continue;
			for (SIZE_T off = 0; off < mbi.RegionSize; off += buf.size()) {
				SIZE_T n = std::min<SIZE_T>(buf.size(), mbi.RegionSize - off);
				auto*  base = static_cast<uint8_t*>(mbi.BaseAddress) + off;
				if (!SafeRead(buf.data(), base, n)) break;
				const auto* q = reinterpret_cast<const uintptr_t*>(buf.data());
				for (SIZE_T i = 0; i < n / 8; ++i) {
					if (q[i] != want) continue;
					uintptr_t obj = reinterpret_cast<uintptr_t>(base) + i * 8;
					if (!ValidCamera(obj)) continue;
					if (!first) first = obj;
					++found;
				}
			}
		}
		g_followCam = first;
		float fov = 0;
		Read(first + kCamFov, fov);
		Log("capture: follow camera %p (%d found in %.1f s), field of view %.1f deg", reinterpret_cast<void*>(first), found, (GetTickCount64() - t0) / 1000.0,
			fov * 57.2958f);
	}

	inline DWORD WINAPI CamScanThread(void*)
	{
		FindCamera();
		g_camScanning = false;
		return 0;
	}

	// ---- device-side hooks -------------------------------------------------------------------------------
	inline void STDMETHODCALLTYPE CreateDsvDetour(ID3D12Device* a_dev, ID3D12Resource* a_res, const D3D12_DEPTH_STENCIL_VIEW_DESC* a_desc, D3D12_CPU_DESCRIPTOR_HANDLE a_h)
	{
		g_origCreateDsv(a_dev, a_res, a_desc, a_h);
		AcquireSRWLockExclusive(&g_dsvLock);
		g_dsvs[a_h.ptr] = a_res;
		ReleaseSRWLockExclusive(&g_dsvLock);
	}

	inline void STDMETHODCALLTYPE ExecuteDetour(ID3D12CommandQueue* a_q, UINT a_n, ID3D12CommandList* const* a_lists)
	{
		if (!t_ours) {
			D3D12_COMMAND_QUEUE_DESC d = a_q->GetDesc();
			if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && g_presentQueue.load(std::memory_order_relaxed) != a_q) {
				static std::atomic<int> s_switches{ 0 };
				a_q->AddRef();  // kept for good: the capture submits on it
				g_presentQueue.store(a_q, std::memory_order_release);
				if (s_switches.fetch_add(1) < 4) Log("capture: the game's direct queue is %p", static_cast<void*>(a_q));
			}
			if (Probing()) ProbeLog("  execute queue %p type %d, %u lists", static_cast<void*>(a_q), static_cast<int>(d.Type), a_n);
		}
		g_origExecute(a_q, a_n, a_lists);
	}

	inline void NoteDepth(ID3D12Resource* a_res, bool a_writeEnded)
	{
		D3D12_RESOURCE_DESC d = a_res->GetDesc();
		uint64_t            frame = g_presents.load(std::memory_order_relaxed);
		AcquireSRWLockExclusive(&g_candLock);
		DepthInfo* c = nullptr;
		for (int i = 0; i < g_candCount && !c; ++i) {
			if (g_cands[i].res == a_res) c = &g_cands[i];
		}
		if (!c) {
			int slot = 0;
			if (g_candCount < 16) {
				slot = g_candCount++;
			} else {
				for (int i = 1; i < 16; ++i) {  // full: reuse the stalest
					if (g_cands[i].lastFrame < g_cands[slot].lastFrame) slot = i;
				}
			}
			c = &g_cands[slot];
			*c = { a_res, d, frame, 0, 0 };
		}
		if (c->desc.Width != d.Width || c->desc.Height != d.Height || c->desc.Format != d.Format) *c = { a_res, d, frame, 0, 0 };
		if (c->lastFrame != frame) {
			c->endsLast = c->lastFrame + 1 == frame ? c->ends : 0;
			c->ends = 0;
			c->lastFrame = frame;
		}
		if (a_writeEnded) ++c->ends;
		ReleaseSRWLockExclusive(&g_candLock);
	}

	inline bool SeenRecently(ID3D12Resource* a_res, uint64_t a_frame)
	{
		bool seen = false;
		AcquireSRWLockShared(&g_candLock);
		for (int i = 0; i < g_candCount && !seen; ++i) seen = g_cands[i].res == a_res && g_cands[i].lastFrame + 2 >= a_frame;
		ReleaseSRWLockShared(&g_candLock);
		return seen;
	}

	// Copies the depth plane into the target's buffer, in a_list, from state a_state (and back).
	inline void RecordDepthCopy(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_res, UINT a_sub, D3D12_RESOURCE_STATES a_state, const DepthTarget* a_dt)
	{
		bool                   move = !(a_state & D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = a_res;
		b.Transition.Subresource = a_sub;
		b.Transition.StateBefore = a_state;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		if (move) g_origBarrier(a_list, 1, &b);
		D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
		dst.pResource = a_dt->buf;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = a_dt->fp;
		src.pResource = a_res;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;
		a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		if (move) {
			std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
			g_origBarrier(a_list, 1, &b);
		}
		g_copiesThisFrame.fetch_add(1, std::memory_order_relaxed);
	}

	inline void STDMETHODCALLTYPE BarrierDetour(ID3D12GraphicsCommandList* a_list, UINT a_n, const D3D12_RESOURCE_BARRIER* a_b)
	{
		g_origBarrier(a_list, a_n, a_b);
		if (t_ours || !a_b) return;
		bool probing = Probing();
		bool watching = probing || Enabled() || g_dumpRequest.load(std::memory_order_relaxed);
		if (!watching) return;
		constexpr D3D12_RESOURCE_STATES kDepth = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
		DepthTarget* dt = g_depthTarget.load(std::memory_order_acquire);
		for (UINT i = 0; i < a_n; ++i) {
			if (a_b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || (a_b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)) continue;
			const auto& t = a_b[i].Transition;
			if (!t.pResource) continue;
			bool whole = t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || t.Subresource == 0;
			if (dt && t.pResource == dt->game && whole) g_depthState.store(t.StateAfter, std::memory_order_relaxed);
			if (!((t.StateBefore | t.StateAfter) & kDepth)) continue;
			bool ended = (t.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) && !(t.StateAfter & D3D12_RESOURCE_STATE_DEPTH_WRITE);
			NoteDepth(t.pResource, ended);
			if (probing) {
				D3D12_RESOURCE_DESC d = t.pResource->GetDesc();
				ProbeLog("  barrier list %p res %p (%llux%u fmt %d flags %x) %x -> %x sub %x%s", static_cast<void*>(a_list), static_cast<void*>(t.pResource),
					static_cast<unsigned long long>(d.Width), d.Height, static_cast<int>(d.Format), static_cast<unsigned>(d.Flags), static_cast<unsigned>(t.StateBefore),
					static_cast<unsigned>(t.StateAfter), t.Subresource, (a_b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) ? " (end of split)" : "");
			}
			if (g_point == 1 && ended && dt && t.pResource == dt->game && whole) {
				D3D12_RESOURCE_DESC d = t.pResource->GetDesc();
				if (d.Width == dt->desc.Width && d.Height == dt->desc.Height && d.Format == dt->desc.Format)
					RecordDepthCopy(a_list, t.pResource, t.Subresource, t.StateAfter, dt);
			}
		}
	}

	inline void STDMETHODCALLTYPE SetRtsDetour(ID3D12GraphicsCommandList* a_list, UINT a_n, const D3D12_CPU_DESCRIPTOR_HANDLE* a_rts, BOOL a_single,
		const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv)
	{
		g_origSetRts(a_list, a_n, a_rts, a_single, a_dsv);
		if (a_dsv && !t_ours && Probing())
			ProbeLog("  set targets list %p: %u colour, depth %p", static_cast<void*>(a_list), a_n, static_cast<void*>(DsvResource(a_dsv->ptr)));
	}

	inline void STDMETHODCALLTYPE ClearDsvDetour(ID3D12GraphicsCommandList* a_list, D3D12_CPU_DESCRIPTOR_HANDLE a_dsv, D3D12_CLEAR_FLAGS a_flags, FLOAT a_depth,
		UINT8 a_stencil, UINT a_nRects, const D3D12_RECT* a_rects)
	{
		if (!t_ours && Probing())
			ProbeLog("  clear depth list %p res %p flags %x depth %.3f stencil %u rects %u", static_cast<void*>(a_list), static_cast<void*>(DsvResource(a_dsv.ptr)),
				static_cast<unsigned>(a_flags), a_depth, a_stencil, a_nRects);
		g_origClearDsv(a_list, a_dsv, a_flags, a_depth, a_stencil, a_nRects, a_rects);
	}

	inline void STDMETHODCALLTYPE BeginPassDetour(ID3D12GraphicsCommandList4* a_list, UINT a_n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* a_rts,
		const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* a_ds, D3D12_RENDER_PASS_FLAGS a_flags)
	{
		if (a_ds && !t_ours && Probing())
			ProbeLog("  render pass list %p: %u colour, depth %p begin %d end %d clear %.3f", static_cast<void*>(a_list), a_n,
				static_cast<void*>(DsvResource(a_ds->cpuDescriptor.ptr)), static_cast<int>(a_ds->DepthBeginningAccess.Type),
				static_cast<int>(a_ds->DepthEndingAccess.Type), a_ds->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth);
		g_origBeginPass(a_list, a_n, a_rts, a_ds, a_flags);
	}

	// Patches the command list, queue and device classes from throwaway objects of the game's device.
	inline void InstallDeviceHooks(ID3D12Device* a_dev)
	{
		if (g_hooked.exchange(true)) return;
		ID3D12Device* dev = a_dev;
		bool          own = false;
		std::string   mod = ModuleOf(**reinterpret_cast<void***>(a_dev));
		if (_stricmp(mod.c_str(), "d3d12.dll") != 0 && _stricmp(mod.c_str(), "D3D12Core.dll") != 0) {
			// a wrapper (Streamline): take the real device - the adapter's one and only - from d3d12.dll itself
			auto create = reinterpret_cast<CreateDeviceFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice"));
			if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
				Log("capture: device %p comes from %s and no real device could be made: no capture", static_cast<void*>(a_dev), mod.c_str());
				return;
			}
			own = true;
		}
		ID3D12CommandQueue*        queue = nullptr;
		ID3D12CommandAllocator*    alloc = nullptr;
		ID3D12GraphicsCommandList* list = nullptr;
		D3D12_COMMAND_QUEUE_DESC   qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
		if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
			SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) {
			list->Close();
			g_origBarrier = reinterpret_cast<BarrierFn>(PatchVtableSlot(list, kListBarrier, reinterpret_cast<void*>(&BarrierDetour)));
			g_origSetRts = reinterpret_cast<SetRtsFn>(PatchVtableSlot(list, kListSetRts, reinterpret_cast<void*>(&SetRtsDetour)));
			g_origClearDsv = reinterpret_cast<ClearDsvFn>(PatchVtableSlot(list, kListClearDsv, reinterpret_cast<void*>(&ClearDsvDetour)));
			ID3D12GraphicsCommandList4* l4 = nullptr;
			if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l4)))) {
				g_origBeginPass = reinterpret_cast<BeginPassFn>(PatchVtableSlot(l4, kListBeginRenderPass, reinterpret_cast<void*>(&BeginPassDetour)));
				l4->Release();
			}
			g_origExecute = reinterpret_cast<ExecuteFn>(PatchVtableSlot(queue, kQueueExecute, reinterpret_cast<void*>(&ExecuteDetour)));
			g_origCreateDsv = reinterpret_cast<CreateDsvFn>(PatchVtableSlot(dev, kDevCreateDsv, reinterpret_cast<void*>(&CreateDsvDetour)));
		}
		Log("capture: D3D12 hooks on device %p (%s%s): barrier %s, set targets %s, clear depth %s, render pass %s, execute %s, depth views %s",
			static_cast<void*>(dev), mod.c_str(), own ? ", real device taken from d3d12.dll" : "", g_origBarrier ? "ok" : "MISSING", g_origSetRts ? "ok" : "MISSING",
			g_origClearDsv ? "ok" : "MISSING", g_origBeginPass ? "ok" : "none", g_origExecute ? "ok" : "MISSING", g_origCreateDsv ? "ok" : "MISSING");
		if (list) list->Release();
		if (alloc) alloc->Release();
		if (queue) queue->Release();
		if (own) dev->Release();
	}

	// The game asks GetProcAddress for D3D12CreateDevice (main.cpp hands out this one instead).
	inline HRESULT WINAPI CreateDeviceDetour(IUnknown* a_adapter, D3D_FEATURE_LEVEL a_level, REFIID a_riid, void** a_out)
	{
		HRESULT hr = g_realCreateDevice(a_adapter, a_level, a_riid, a_out);
		if (SUCCEEDED(hr) && a_out && *a_out) {
			ID3D12Device* dev = nullptr;
			if (SUCCEEDED(static_cast<IUnknown*>(*a_out)->QueryInterface(IID_PPV_ARGS(&dev)))) {
				InstallDeviceHooks(dev);
				dev->Release();
			}
		}
		return hr;
	}

	// ---- present side ----------------------------------------------------------------------------------
	inline void WaitGpu()
	{
		ID3D12Fence* f = g_fence.load();
		if (!f || f->GetCompletedValue() >= g_fenceValue) return;
		HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (e && SUCCEEDED(f->SetEventOnCompletion(g_fenceValue, e))) WaitForSingleObject(e, 1000);
		if (e) CloseHandle(e);
	}

	inline bool CreateStatic()
	{
		auto serialize = reinterpret_cast<SerializeRootSigFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		if (!serialize) return false;
		D3D12_DESCRIPTOR_RANGE ranges[2] = {
			{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 },
			{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 },
		};
		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = { 0, 0, 10 };
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 2, ranges };
		D3D12_ROOT_SIGNATURE_DESC rs{ 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ID3DBlob* blob = nullptr;
		ID3DBlob* err = nullptr;
		if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
			Log("capture: root signature: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
			if (err) err->Release();
			return false;
		}
		HRESULT hr = g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
		blob->Release();
		if (FAILED(hr)) return false;
		D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = g_rootSig;
		pd.CS = { g_captureCs, sizeof(g_captureCs) };
		if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) return false;
		D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * proto::kCaptureSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
		g_descSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		for (auto& f : g_frames) {
			if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
		}
		if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_list)))) return false;
		g_list->Close();
		ID3D12Fence* fence = nullptr;
		if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
		g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g_fence.store(fence);
		return true;
	}

	// A D3D11 device on the game's D3D12 device and queue: it makes the textures Arkham can open.
	inline bool Make11On12(ID3D12CommandQueue* a_queue)
	{
		if (g_11on12) return true;
		if (g_11on12Failed) return false;
		auto     create = reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(GetProcAddress(LoadLibraryW(L"d3d11.dll"), "D3D11On12CreateDevice"));
		IUnknown* queues[] = { a_queue };
		HRESULT  hr = create ? create(g_dev, 0, nullptr, 0, queues, 1, 0, &g_dev11, &g_ctx11, nullptr) : E_NOINTERFACE;
		if (SUCCEEDED(hr)) hr = g_dev11->QueryInterface(IID_PPV_ARGS(&g_11on12));
		if (FAILED(hr)) {
			g_11on12Failed = true;
			Log("capture: D3D11On12 device failed (0x%08lx) - sharing by name instead", static_cast<unsigned long>(hr));
			return false;
		}
		Log("capture: D3D11On12 device %p on queue %p", static_cast<void*>(g_dev11), static_cast<void*>(a_queue));
		return true;
	}

	inline D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT a_i)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(a_i) * g_descSize;
		return h;
	}

	inline D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT a_i)
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = g_heap->GetGPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<UINT64>(a_i) * g_descSize;
		return h;
	}

	inline ID3D12Resource* MakeBuffer(UINT64 a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state)
	{
		D3D12_HEAP_PROPERTIES hp{ a_heap };
		D3D12_RESOURCE_DESC   d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = a_bytes;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource* r = nullptr;
		return SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, a_state, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
	}

	inline void ReleaseSized()
	{
		if (g_privColor) g_privColor->Release(), g_privColor = nullptr;
		for (UINT i = 0; i < proto::kCaptureSlots; ++i) {
			if (g_wrapped[i]) g_wrapped[i]->Release(), g_wrapped[i] = nullptr;
			if (g_shared11[i]) g_shared11[i]->Release(), g_shared11[i] = nullptr;
			g_legacy[i] = nullptr;  // owned by the texture
			if (g_outTex[i]) g_outTex[i]->Release(), g_outTex[i] = nullptr;
			if (g_outHandle[i]) CloseHandle(g_outHandle[i]), g_outHandle[i] = nullptr;
		}
		if (g_ctx11) g_ctx11->Flush();  // 11on12 lets go of the wrapped resources
		if (g_rbColor) g_rbColor->Release(), g_rbColor = nullptr;
		g_bbW = g_bbH = g_w = g_h = 0;
	}

	// Back-buffer sized: a private copy of the frame. View sized: the outputs (compute targets) and how
	// they are shared - D3D11 textures with legacy handles (11on12), or the outputs themselves by name.
	inline bool Recreate(const D3D12_RESOURCE_DESC& a_bb, UINT a_x0, UINT a_y0, UINT a_w, UINT a_h, ID3D12CommandQueue* a_queue)
	{
		WaitGpu();
		ReleaseSized();
		++g_gen;
		proto::CaptureShare share = g_share == proto::kShareLegacy && Make11On12(a_queue) ? proto::kShareLegacy : proto::kShareNamed;
		D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC   d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = a_bb.Width;
		d.Height = a_bb.Height;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = a_bb.Format;
		d.SampleDesc.Count = 1;
		if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_privColor)))) return false;
		d.Width = a_w;
		d.Height = a_h;
		d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | (share == proto::kShareNamed ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS : D3D12_RESOURCE_FLAG_NONE);
		DWORD pid = GetCurrentProcessId();
		for (UINT i = 0; i < proto::kCaptureSlots; ++i) {
			if (share == proto::kShareNamed) {
				if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_outTex[i])))) return false;
				wchar_t name[64];
				swprintf_s(name, proto::kCaptureNameFmt, static_cast<unsigned>(pid), g_gen, i);
				if (FAILED(g_dev->CreateSharedHandle(g_outTex[i], nullptr, GENERIC_ALL, name, &g_outHandle[i]))) {
					Log("capture: can't share %ls", name);
					return false;
				}
			} else {
				// the compute target stays in UNORDERED_ACCESS; 11on12 copies it into a shared D3D11 texture
				if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_outTex[i]))))
					return false;
				D3D11_RESOURCE_FLAGS rf{ D3D11_BIND_SHADER_RESOURCE, 0, 0, 0 };
				if (FAILED(g_11on12->CreateWrappedResource(g_outTex[i], &rf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
						IID_PPV_ARGS(&g_wrapped[i]))))
					return false;
				D3D11_TEXTURE2D_DESC td{ a_w, a_h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
					D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED };
				if (FAILED(g_dev11->CreateTexture2D(&td, nullptr, &g_shared11[i]))) return false;
				IDXGIResource* r = nullptr;
				HRESULT        hr = g_shared11[i]->QueryInterface(IID_PPV_ARGS(&r));
				if (SUCCEEDED(hr)) hr = r->GetSharedHandle(&g_legacy[i]);
				if (r) r->Release();
				if (FAILED(hr)) return false;
			}
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			g_dev->CreateUnorderedAccessView(g_outTex[i], nullptr, &uav, Cpu(i * 3 + 2));
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = a_bb.Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			g_dev->CreateShaderResourceView(g_privColor, &srv, Cpu(i * 3));
		}
		g_madeShare = share;
		g_bbW = static_cast<UINT>(a_bb.Width), g_bbH = a_bb.Height, g_colorFmt = a_bb.Format;
		g_w = a_w, g_h = a_h, g_x0 = a_x0, g_y0 = a_y0;
		g_boundDepth = nullptr;
		Log("capture: frame %ux%u format %d, 3D view %ux%u at (%u %u), textures generation %u shared %s", g_bbW, g_bbH, static_cast<int>(g_colorFmt), g_w, g_h,
			g_x0, g_y0, g_gen, share == proto::kShareLegacy ? "with legacy handles (11on12)" : "by name");
		return true;
	}

	// Which depth buffer to copy: the one asked for, else the largest one in the depth formats that
	// ended writes this frame or last, between a third of the frame and twice its size each way.
	inline void PickDepth(uint64_t a_frame, UINT a_bbW, UINT a_bbH)
	{
		DepthInfo best{};
		AcquireSRWLockShared(&g_candLock);
		for (int i = 0; i < g_candCount; ++i) {
			const DepthInfo& c = g_cands[i];
			if (g_depthPick) {
				if (reinterpret_cast<uintptr_t>(c.res) == g_depthPick) best = c;
				continue;
			}
			bool recent = c.lastFrame + 2 >= a_frame && (c.ends || c.endsLast);
			if (!recent || !IsDepthFamily(c.desc.Format) || c.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || c.desc.DepthOrArraySize != 1 ||
				c.desc.SampleDesc.Count != 1 || c.desc.Width * 3 < a_bbW || c.desc.Height * 3 < a_bbH || c.desc.Width > 2ull * a_bbW || c.desc.Height > 2u * a_bbH)
				continue;
			if (!best.res || c.desc.Width * c.desc.Height > best.desc.Width * best.desc.Height) best = c;
		}
		ReleaseSRWLockShared(&g_candLock);
		DepthTarget* cur = g_depthTarget.load();
		if (!best.res) return;
		if (cur && cur->game == best.res && cur->desc.Width == best.desc.Width && cur->desc.Height == best.desc.Height && cur->desc.Format == best.desc.Format) return;
		auto* t = new DepthTarget{ best.res, best.desc, {}, 0, nullptr };
		g_dev->GetCopyableFootprints(&t->desc, 0, 1, 0, &t->fp, nullptr, nullptr, &t->bytes);
		t->buf = MakeBuffer(t->bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
		if (!t->buf) {
			delete t;
			return;
		}
		Log("capture: depth buffer %p (%llux%u format %d), copy %llu bytes, row %u bytes, footprint format %d", static_cast<void*>(best.res),
			static_cast<unsigned long long>(best.desc.Width), best.desc.Height, static_cast<int>(best.desc.Format), static_cast<unsigned long long>(t->bytes),
			t->fp.Footprint.RowPitch, static_cast<int>(t->fp.Footprint.Format));
		g_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;  // until a barrier says otherwise
		g_depthTarget.store(t, std::memory_order_release);
		if (cur) g_graveyard.push_back({ cur, a_frame });
	}

	inline void BuryOld(uint64_t a_frame)
	{
		for (size_t i = 0; i < g_graveyard.size();) {
			if (a_frame - g_graveyard[i].frame > 300) {
				if (g_graveyard[i].t->buf) g_graveyard[i].t->buf->Release();
				delete g_graveyard[i].t;
				g_graveyard[i] = g_graveyard.back();
				g_graveyard.pop_back();
			} else {
				++i;
			}
		}
	}

	inline void Transition(ID3D12Resource* a_r, D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to, D3D12_RESOURCE_BARRIER& a_out)
	{
		a_out = {};
		a_out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		a_out.Transition = { a_r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a_from, a_to };
	}

	inline void Record(ID3D12CommandQueue* a_queue, ID3D12Resource* a_bb, uint64_t a_frame)
	{
		D3D12_RESOURCE_DESC bd = a_bb->GetDesc();
		if (!g_dev) {
			a_bb->GetDevice(IID_PPV_ARGS(&g_dev));
			if (!g_dev || !CreateStatic()) {
				g_staticFailed = true;
				Log("capture: could not set up (device %p) - capture off", static_cast<void*>(g_dev));
				return;
			}
			Log("capture: compute pass ready on device %p", static_cast<void*>(g_dev));
		}
		PickDepth(a_frame, static_cast<UINT>(bd.Width), bd.Height);
		BuryOld(a_frame);
		DepthTarget* dt = g_depthTarget.load();
		if (!dt || !SeenRecently(dt->game, a_frame)) return;  // no depth buffer seen (yet, or any more)
		// the 3D view inside the back buffer: the depth buffer's shape, letterboxed or pillarboxed
		double a = static_cast<double>(dt->desc.Width) / dt->desc.Height;
		UINT   vw = static_cast<UINT>(bd.Width), vh = bd.Height;
		if (static_cast<double>(bd.Width) / bd.Height > a) vw = std::min(vw, static_cast<UINT>(bd.Height * a + 0.5));
		else vh = std::min(vh, static_cast<UINT>(bd.Width / a + 0.5));
		UINT x0 = (static_cast<UINT>(bd.Width) - vw) / 2, y0 = (bd.Height - vh) / 2;
		if (bd.Width != g_bbW || bd.Height != g_bbH || bd.Format != g_colorFmt || vw != g_w || vh != g_h || g_share != g_madeShare) {
			if (!Recreate(bd, x0, y0, vw, vh, a_queue)) {
				g_staticFailed = true;
				Log("capture: could not make the frame textures - capture off");
				ReleaseSized();
				return;
			}
		}
		if (dt != g_boundDepth) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = static_cast<UINT>(dt->bytes / 4);
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			for (UINT i = 0; i < proto::kCaptureSlots; ++i) g_dev->CreateShaderResourceView(dt->buf, &srv, Cpu(i * 3 + 1));
			g_boundDepth = dt;
		}
		ID3D12Fence* fence = g_fence.load();
		FrameSlot&   f = g_frames[a_frame % 3];
		if (fence->GetCompletedValue() < f.fence) {
			g_framesSkipped.fetch_add(1, std::memory_order_relaxed);  // the GPU is still on this slot's last frame
			return;
		}
		bool dump = g_dumpRequest.exchange(false);
		if (dump) {
			D3D12_RESOURCE_DESC cd = g_privColor->GetDesc();
			g_dev->GetCopyableFootprints(&cd, 0, 1, 0, &g_rbColorFp, nullptr, nullptr, &g_rbColorBytes);
			if (g_rbColor) g_rbColor->Release();
			if (g_rbDepth) g_rbDepth->Release();
			g_rbColor = MakeBuffer(g_rbColorBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
			g_rbDepth = MakeBuffer(dt->bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
			g_rbDepthBytes = dt->bytes;
			g_rbDepthOf = *dt;
			g_dumpRect[0] = g_x0, g_dumpRect[1] = g_y0, g_dumpRect[2] = g_w, g_dumpRect[3] = g_h;
			dump = g_rbColor && g_rbDepth;
		}
		f.alloc->Reset();
		g_list->Reset(f.alloc, nullptr);
		if (g_point == 0) RecordDepthCopy(g_list, dt->game, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, g_depthState.load(), dt);  // the whole frame's depth
		D3D12_RESOURCE_BARRIER b[2];
		Transition(a_bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE, b[0]);
		g_origBarrier(g_list, 1, b);
		g_list->CopyResource(g_privColor, a_bb);
		Transition(a_bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT, b[0]);
		Transition(g_privColor, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, b[1]);
		g_origBarrier(g_list, 2, b);
		if (g_point == 0) {
			// the depth copy above was a write to the buffer the compute pass reads
			D3D12_RESOURCE_BARRIER ub{};
			ub.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			ub.Transition = { dt->buf, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
			g_origBarrier(g_list, 1, &ub);
		}
		struct
		{
			UINT  outW, outH, depW, depH, depPitch;
			float nearZ, range;
			UINT  reversed, colX0, colY0;
		} p{ g_w, g_h, static_cast<UINT>(dt->desc.Width), dt->desc.Height, dt->fp.Footprint.RowPitch, g_near, g_range, g_reversed ? 1u : 0u, g_x0, g_y0 };
		float lensFov, lensNear;
		if (CameraLens(lensFov, lensNear)) p.nearZ = lensNear;
		ID3D12DescriptorHeap* heaps[] = { g_heap };
		g_list->SetDescriptorHeaps(1, heaps);
		g_list->SetComputeRootSignature(g_rootSig);
		g_list->SetPipelineState(g_pso);
		g_list->SetComputeRoot32BitConstants(0, 10, &p, 0);
		g_list->SetComputeRootDescriptorTable(1, Gpu(g_slot * 3));
		g_list->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
		if (dump) {
			Transition(g_privColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, b[0]);
			g_origBarrier(g_list, 1, b);
			D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
			dst.pResource = g_rbColor;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint = g_rbColorFp;
			src.pResource = g_privColor;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			if (g_point == 0) {
				D3D12_RESOURCE_BARRIER ub{};
				ub.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				ub.Transition = { dt->buf, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE };
				g_origBarrier(g_list, 1, &ub);
			}
			g_list->CopyBufferRegion(g_rbDepth, 0, dt->buf, 0, dt->bytes);
			Transition(g_privColor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, b[0]);
		} else {
			Transition(g_privColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST, b[0]);
		}
		g_origBarrier(g_list, 1, b);
		g_list->Close();
		t_ours = true;
		ID3D12CommandList* lists[] = { g_list };
		a_queue->ExecuteCommandLists(1, lists);
		if (g_madeShare == proto::kShareLegacy) {
			// 11on12 records the copy into the shared D3D11 texture and submits it on the same queue
			g_11on12->AcquireWrappedResources(&g_wrapped[g_slot], 1);
			g_ctx11->CopyResource(g_shared11[g_slot], g_wrapped[g_slot]);
			g_11on12->ReleaseWrappedResources(&g_wrapped[g_slot], 1);
			g_ctx11->Flush();
		}
		f.fence = ++g_fenceValue;
		a_queue->Signal(fence, f.fence);
		t_ours = false;

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		Pending pe{ f.fence, g_slot, g_gen, now.QuadPart, {}, {}, 0.0f, 0.0f };
		if (g_readCamera) g_readCamera(pe.camPos, pe.camRot);
		LensTan(static_cast<float>(g_w) / g_h, pe.tanX, pe.tanY);
		AcquireSRWLockExclusive(&g_pendLock);
		g_pend[f.fence % 8] = pe;
		ReleaseSRWLockExclusive(&g_pendLock);
		if (dump) {
			g_dumpTan[0] = pe.tanX, g_dumpTan[1] = pe.tanY;
			g_dumpFov = CameraLens(lensFov, lensNear) ? lensFov : 0.0f;
			g_dumpFence = f.fence;
		}
		fence->SetEventOnCompletion(f.fence, g_fenceEvent);
		g_slot = (g_slot + 1) % proto::kCaptureSlots;
	}

	inline void OnPresent(IDXGISwapChain* a_swap)
	{
		uint64_t frame = g_presents.fetch_add(1) + 1;
		g_copiesLastFrame = g_copiesThisFrame.exchange(0);
		if (int left = g_probeFrames.load(); left > 0) {
			ProbeLog("probe: present %llu on swap chain %p (depth copies this frame %u); queue %p", static_cast<unsigned long long>(frame), static_cast<void*>(a_swap),
				g_copiesLastFrame.load(), static_cast<void*>(g_presentQueue.load()));
			g_probeLines = 0;
			if (g_probeFrames.fetch_sub(1) == 1) {
				AcquireSRWLockShared(&g_candLock);
				for (int i = 0; i < g_candCount; ++i) {
					const DepthInfo& c = g_cands[i];
					Log("probe: depth candidate %p %llux%u format %d flags %x, writes ended %u/%u (this/last frame), seen frame %llu", static_cast<void*>(c.res),
						static_cast<unsigned long long>(c.desc.Width), c.desc.Height, static_cast<int>(c.desc.Format), static_cast<unsigned>(c.desc.Flags), c.ends,
						c.endsLast, static_cast<unsigned long long>(c.lastFrame));
				}
				ReleaseSRWLockShared(&g_candLock);
				Log("probe: done");
			}
		}
		if (g_staticFailed || (!Enabled() && !g_dumpRequest.load(std::memory_order_relaxed))) return;
		ID3D12CommandQueue* queue = g_presentQueue.load(std::memory_order_acquire);
		if (!queue) return;
		IDXGISwapChain3* s3 = nullptr;
		if (FAILED(a_swap->QueryInterface(IID_PPV_ARGS(&s3)))) return;
		ID3D12Resource* bb = nullptr;
		s3->GetBuffer(s3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
		s3->Release();
		if (!bb) return;
		Record(queue, bb, frame);
		bb->Release();
	}

	inline HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* a_swap, UINT a_sync, UINT a_flags)
	{
		if (!(a_flags & DXGI_PRESENT_TEST)) OnPresent(a_swap);
		HRESULT hr = g_origPresent(a_swap, a_sync, a_flags);
		if (hr == DXGI_STATUS_OCCLUDED && Enabled()) {  // hidden behind Arkham: keep it rendering
			if (g_occluded.fetch_add(1) == 0) Log("capture: Present reports the window occluded - told the game it isn't");
			hr = S_OK;
		}
		return hr;
	}

	inline HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* a_swap, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_p)
	{
		if (!(a_flags & DXGI_PRESENT_TEST)) OnPresent(a_swap);
		HRESULT hr = g_origPresent1(a_swap, a_sync, a_flags, a_p);
		if (hr == DXGI_STATUS_OCCLUDED && Enabled()) {
			if (g_occluded.fetch_add(1) == 0) Log("capture: Present1 reports the window occluded - told the game it isn't");
			hr = S_OK;
		}
		return hr;
	}

	inline HRESULT STDMETHODCALLTYPE ResizeDetour(IDXGISwapChain* a_swap, UINT a_n, UINT a_w, UINT a_h, DXGI_FORMAT a_f, UINT a_flags)
	{
		Log("capture: the game resizes its swap chain to %ux%u", a_w, a_h);
		return g_origResize(a_swap, a_n, a_w, a_h, a_f, a_flags);  // we hold no back buffer between frames
	}

	inline void InstallSwapHooks()
	{
		if (g_swapHooked.exchange(true)) return;
		auto createFactory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1"));
		auto createDevice = reinterpret_cast<CreateDeviceFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice"));
		IDXGIFactory2*      factory = nullptr;
		ID3D12Device*       dev = nullptr;
		ID3D12CommandQueue* queue = nullptr;
		IDXGISwapChain1*    swap = nullptr;
		WNDCLASSW           wc{ 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"ArkWebCaptureDummy" };
		RegisterClassW(&wc);
		HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
		D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
		DXGI_SWAP_CHAIN_DESC1    sd{};
		sd.Width = sd.Height = 64;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		if (createFactory && createDevice && wnd && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) &&
			SUCCEEDED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) && SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
			SUCCEEDED(factory->CreateSwapChainForHwnd(queue, wnd, &sd, nullptr, nullptr, &swap))) {
			g_origPresent = reinterpret_cast<PresentFn>(PatchVtableSlot(swap, kSwapPresent, reinterpret_cast<void*>(&PresentDetour)));
			g_origPresent1 = reinterpret_cast<Present1Fn>(PatchVtableSlot(swap, kSwapPresent1, reinterpret_cast<void*>(&Present1Detour)));
			g_origResize = reinterpret_cast<ResizeFn>(PatchVtableSlot(swap, kSwapResizeBuffers, reinterpret_cast<void*>(&ResizeDetour)));
		}
		Log("capture: swap chain hooks (%s): Present %s, Present1 %s, ResizeBuffers %s", swap ? ModuleOf(**reinterpret_cast<void***>(swap)).c_str() : "no swap chain",
			g_origPresent ? "ok" : "MISSING", g_origPresent1 ? "ok" : "MISSING", g_origResize ? "ok" : "MISSING");
		if (swap) swap->Release();
		if (queue) queue->Release();
		if (dev) dev->Release();
		if (factory) factory->Release();
		if (wnd) DestroyWindow(wnd);
	}

	// ---- publishing and dumps (capture thread) ----------------------------------------------------------
	inline void Publish()
	{
		ID3D12Fence* fence = g_fence.load();
		if (!fence || !g_state) return;
		UINT64  done = fence->GetCompletedValue();
		Pending best{};
		AcquireSRWLockShared(&g_pendLock);
		for (const Pending& p : g_pend) {
			if (p.fence && p.fence <= done && p.fence > g_publishedFence && p.fence > best.fence) best = p;
		}
		ReleaseSRWLockShared(&g_pendLock);
		ULONGLONG now = GetTickCount64();
		proto::CaptureState cs{};
		if (best.fence) {
			g_publishedFence = best.fence;
			g_lastPublishMs = now;
			g_framesDone.fetch_add(1, std::memory_order_relaxed);
			cs.flags = proto::kCapLive;
			cs.generation = best.gen;
			cs.slot = best.slot;
			cs.frameId = best.fence;
			cs.qpc = best.qpc;
			cs.width = g_w, cs.height = g_h;
			cs.tanHalfFovX = best.tanX, cs.tanHalfFovY = best.tanY;
			memcpy(cs.camPos, best.camPos, sizeof(cs.camPos));
			memcpy(cs.camRot, best.camRot, sizeof(cs.camRot));
			cs.range = g_range;
			cs.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			cs.share = g_madeShare;
			for (UINT i = 0; i < proto::kCaptureSlots; ++i) cs.handles[i] = reinterpret_cast<uint64_t>(g_legacy[i]);
			SeqWrite(g_state, cs);
		} else if (g_lastPublishMs && now - g_lastPublishMs > 500) {
			g_lastPublishMs = 0;  // no frames anymore: the host stops drawing
			SeqWrite(g_state, cs);
		}
	}

	inline void WriteDump()
	{
		UINT64       want = g_dumpFence.load();
		ID3D12Fence* fence = g_fence.load();
		if (!want || !fence || fence->GetCompletedValue() < want) return;
		g_dumpFence = 0;
		std::wstring dir = g_logDir + L"\\capture";
		CreateDirectoryW(dir.c_str(), nullptr);
		void*       p = nullptr;
		D3D12_RANGE none{ 0, 0 };
		D3D12_RANGE all{ 0, static_cast<SIZE_T>(g_rbColorBytes) };
		if (SUCCEEDED(g_rbColor->Map(0, &all, &p))) {
			if (FILE* f = _wfopen((dir + L"\\color.bin").c_str(), L"wb")) {
				fwrite(p, 1, all.End, f);
				fclose(f);
			}
			g_rbColor->Unmap(0, &none);
		} else {
			Log("capture: can't read the colour back");
		}
		D3D12_RANGE dall{ 0, static_cast<SIZE_T>(g_rbDepthBytes) };
		if (SUCCEEDED(g_rbDepth->Map(0, &dall, &p))) {
			if (FILE* f = _wfopen((dir + L"\\depth.bin").c_str(), L"wb")) {
				fwrite(p, 1, dall.End, f);
				fclose(f);
			}
			g_rbDepth->Unmap(0, &none);
		}
		if (FILE* f = _wfopen((dir + L"\\meta.txt").c_str(), L"w")) {
			fprintf(f, "color %u %u format %d rowpitch %u\n", g_rbColorFp.Footprint.Width, g_rbColorFp.Footprint.Height, static_cast<int>(g_rbColorFp.Footprint.Format),
				g_rbColorFp.Footprint.RowPitch);
			fprintf(f, "depth %llu %u format %d rowpitch %u resource %p\n", static_cast<unsigned long long>(g_rbDepthOf.desc.Width), g_rbDepthOf.desc.Height,
				static_cast<int>(g_rbDepthOf.desc.Format), g_rbDepthOf.fp.Footprint.RowPitch, static_cast<void*>(g_rbDepthOf.game));
			fprintf(f, "near %g reversed %d range %g tan %g %g\n", g_near, g_reversed ? 1 : 0, g_range, g_dumpTan[0], g_dumpTan[1]);
			fprintf(f, "view %u %u %u %u fov %g\n", g_dumpRect[0], g_dumpRect[1], g_dumpRect[2], g_dumpRect[3], g_dumpFov);
			if (g_dumpExtra) g_dumpExtra(f);
			fclose(f);
		}
		Log("capture: dump written to logs\\capture (color.bin, depth.bin, meta.txt); follow camera FOV %.2f deg", g_dumpFov * 57.2958f);
	}

	// ---- `cap findproj`: projection matrices anywhere in the game's memory ---------------------------
	inline DWORD WINAPI FindProjThread(void*)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
		MEMORY_BASIC_INFORMATION mbi;
		std::vector<uint8_t>     buf(1 << 20);
		int                      hits = 0;
		uint64_t                 scanned = 0;
		for (uint8_t* a = nullptr; VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi) && hits < 40; a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
			if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) ||
				!(mbi.Protect & (PAGE_READWRITE)) || mbi.RegionSize > (256u << 20))
				continue;
			for (SIZE_T off = 0; off < mbi.RegionSize && hits < 40; off += buf.size()) {
				SIZE_T n = std::min<SIZE_T>(buf.size(), mbi.RegionSize - off);
				auto*  base = static_cast<uint8_t*>(mbi.BaseAddress) + off;
				if (!SafeRead(buf.data(), base, n)) break;
				scanned += n;
				for (SIZE_T i = 0; i + 64 <= n; i += 4) {
					const float* m = reinterpret_cast<const float*>(buf.data() + i);
					if (!(m[0] > 0.3f && m[0] < 5.0f && m[5] > 0.3f && m[5] < 5.0f) || (m[0] == 1.0f && m[5] == 1.0f)) continue;
					float r = m[5] / m[0];
					if (r < 0.95f || r > 2.6f) continue;
					if (m[1] != 0.0f || m[3] != 0.0f || m[4] != 0.0f || m[7] != 0.0f || m[15] != 0.0f) continue;
					// row vectors: w = +-z from the third row, fourth row (0 0 B 0); column vectors: the transpose
					// (TAA jitter sits in m[8]/m[9] or m[2]/m[6], so those aren't required to be zero)
					bool row = std::fabs(std::fabs(m[11]) - 1.0f) < 1e-6f && m[12] == 0.0f && m[13] == 0.0f;
					bool col = std::fabs(std::fabs(m[14]) - 1.0f) < 1e-6f && m[8] == 0.0f && m[9] == 0.0f;
					if (!row && !col) continue;
					Log("  projection at %p (%s vectors): x %.4f y %.4f (vertical FOV %.1f deg) | %g %g %g %g | %g %g %g %g", static_cast<void*>(base + i),
						row ? "row" : "column", m[0], m[5], 2.0 * std::atan(1.0 / m[5]) * 180.0 / 3.14159265, m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
					++hits;
				}
			}
		}
		Log("  findproj: %d matrices in %.0f MB", hits, scanned / 1048576.0);
		return 0;
	}

	// ---- commands: cap on|off|auto | probe [frames] | dump | depth auto|<addr> | point present|barrier |
	//      share legacy|named | near <m> | reversed 0|1 | range <m> | fov <deg> | fovmode h|v | findcam |
	//      findproj | status
	inline void Command(const char* a_verb, const char* a_a)
	{
		if (!a_verb) a_verb = "status";
		if (!_stricmp(a_verb, "on") || !_stricmp(a_verb, "off") || !_stricmp(a_verb, "auto")) {
			g_mode = !_stricmp(a_verb, "on") ? 1 : !_stricmp(a_verb, "off") ? 0 : 2;
			Log("  capture %s", g_mode == 1 ? "ON" : g_mode == 0 ? "off" : "automatic (on while the hero is on Gotham in the sky)");
		} else if (!_stricmp(a_verb, "probe")) {
			g_probeLines = 0;
			g_probeFrames = a_a ? std::max(1, std::min(5, atoi(a_a))) : 1;
			Log("  probing %d frame(s)", g_probeFrames.load());
		} else if (!_stricmp(a_verb, "dump")) {
			g_dumpRequest = true;
		} else if (!_stricmp(a_verb, "depth") && a_a) {
			g_depthPick = !_stricmp(a_a, "auto") ? 0 : strtoull(a_a, nullptr, 16);
			Log("  depth buffer: %s", g_depthPick ? a_a : "automatic");
		} else if (!_stricmp(a_verb, "point") && a_a) {
			g_point = !_stricmp(a_a, "barrier") ? 1 : 0;
			Log("  depth copied %s", g_point ? "after the first pass that wrote it" : "at Present (the whole frame)");
		} else if (!_stricmp(a_verb, "share") && a_a) {
			g_share = !_stricmp(a_a, "named") ? proto::kShareNamed : proto::kShareLegacy;
			Log("  frames shared %s (from the next frame)", g_share == proto::kShareNamed ? "by name (D3D12)" : "with legacy handles (11on12)");
		} else if (!_stricmp(a_verb, "near") && a_a) {
			g_near = static_cast<float>(atof(a_a));
		} else if (!_stricmp(a_verb, "reversed") && a_a) {
			g_reversed = atoi(a_a) != 0;
		} else if (!_stricmp(a_verb, "range") && a_a) {
			g_range = static_cast<float>(atof(a_a));
		} else if (!_stricmp(a_verb, "fov") && a_a) {
			g_fovYDeg = static_cast<float>(atof(a_a));
		} else if (!_stricmp(a_verb, "fovmode") && a_a) {
			g_fovHorizontal = _stricmp(a_a, "v") != 0;
			Log("  the follow camera's FOV is taken as %s", g_fovHorizontal ? "horizontal" : "vertical");
		} else if (!_stricmp(a_verb, "findcam")) {
			g_followCam = 0;
			g_lastCamScan = 0;
			g_camScanWanted = true;
		} else if (!_stricmp(a_verb, "findproj")) {
			CreateThread(nullptr, 0, FindProjThread, nullptr, 0, nullptr);
		} else {
			DepthTarget* dt = g_depthTarget.load();
			float        fov = 0, nearZ = 0;
			bool         lens = CameraLens(fov, nearZ);
			Log("  capture %s (%s): hooks %s/%s, %llu presents, %llu frames published, %llu skipped (GPU busy), %u depth copies last frame, depth %p state %x, "
				"frame %ux%u view %ux%u at (%u %u) gen %u shared %s, near %g reversed %d range %g, follow camera %p FOV %.1f deg (%s), occluded %llu",
				g_mode == 1 ? "ON" : g_mode == 0 ? "off" : "auto", Enabled() ? "capturing" : "idle", g_hooked ? "device" : "-", g_swapHooked ? "swap chain" : "-",
				static_cast<unsigned long long>(g_presents.load()), static_cast<unsigned long long>(g_framesDone.load()),
				static_cast<unsigned long long>(g_framesSkipped.load()), g_copiesLastFrame.load(), dt ? static_cast<void*>(dt->game) : nullptr,
				static_cast<unsigned>(g_depthState.load()), g_bbW, g_bbH, g_w, g_h, g_x0, g_y0, g_gen, g_madeShare == proto::kShareLegacy ? "legacy" : "named", g_near,
				g_reversed ? 1 : 0, g_range, reinterpret_cast<void*>(g_followCam.load()), lens ? fov * 57.2958f : 0.0f, g_fovHorizontal ? "horizontal" : "vertical",
				static_cast<unsigned long long>(g_occluded.load()));
		}
	}

	inline DWORD WINAPI Thread(void*)
	{
		// the swap chain's hooks once the game has its device and its window
		while (!g_windowReady.load()) Sleep(250);
		for (int i = 0; i < 40 && !g_hooked.load(); ++i) Sleep(250);
		if (!g_hooked.load()) {
			// the device was made before the hooks could see it: hook from the real device now (no depth views known)
			auto create = reinterpret_cast<CreateDeviceFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice"));
			ID3D12Device* dev = nullptr;
			if (create && SUCCEEDED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
				Log("capture: no D3D12CreateDevice seen; hooking late");
				InstallDeviceHooks(dev);
				dev->Release();
			}
		}
		InstallSwapHooks();
		for (;;) {
			if (g_fenceEvent) WaitForSingleObject(g_fenceEvent, 50);
			else Sleep(50);
			Publish();
			WriteDump();
			// the follow camera: looked for (on a thread of its own) once wanted, again every 30 s while missing
			ULONGLONG now = GetTickCount64();
			if (g_camScanWanted.load() && !g_followCam.load() && !g_camScanning.load() && (!g_lastCamScan || now - g_lastCamScan > 30000)) {
				g_lastCamScan = now;
				g_camScanWanted = false;
				g_camScanning = true;
				if (HANDLE h = CreateThread(nullptr, 0, CamScanThread, nullptr, 0, nullptr)) CloseHandle(h);
				else g_camScanning = false;
			}
		}
	}
}
