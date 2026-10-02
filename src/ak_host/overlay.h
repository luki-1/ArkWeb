// Phase V: Spider-Man's own picture of the hero and his webs drawn over Arkham's frame.
//
// The guest leaves each Spider-Man frame, cut down to what is near his camera, in D3D12 textures shared
// by name (protocol CaptureState). Arkham's view already is Spider-Man's camera (OverrideView), so the
// capture lines up with Gotham as is; only the fields of view may differ, which a scale about the
// screen centre takes care of. Drawn at Present (after Arkham's HUD) on Arkham's render thread, inside
// a separate device context state so none of Arkham's pipeline state is touched. While it draws, main.cpp
// hides Batman.
//
// The hook is the IDXGISwapChain Present slot of DXGI's swap chain class, patched from a throwaway
// swap chain (one vtable per class: Arkham's swap chain uses it too).
#pragma once

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "../common/link.h"
#include "../common/util.h"
#include "overlay_ps.h"
#include "overlay_vs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace arkweb::overlay
{
	constexpr int kSwapPresent = 8, kSwapResizeBuffers = 13;  // IDXGISwapChain C vtable (dxgi.h)

	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

	inline PresentFn g_origPresent = nullptr;
	inline ResizeFn  g_origResize = nullptr;

	// ---- settings / inputs -----------------------------------------------------------------------------
	inline std::atomic<bool>     g_enabled{ true };        // `overlay on|off`, ini Overlay
	inline std::atomic<float>    g_akFovDeg{ 0.0f };       // Arkham's (horizontal) field of view this frame - game thread
	inline std::atomic<bool>     g_puppet{ false };        // Batman is the puppet and the game is ticking - game thread
	inline float                 g_fovScale = 1.0f;        // `overlay scale`: Spider-Man's field of view when the guest can't say
	inline float                 g_offset[2] = {};         // `overlay offset`: capture uv shift
	inline float                 g_tint[3] = { 1.0f, 1.0f, 1.0f };  // `overlay tint`
	inline float                 g_gamma = 1.0f;                    // `overlay gamma` (< 1 lifts his shadows)
	inline proto::CaptureState*  g_capture = nullptr;      // set by main.cpp once the link is open
	inline proto::Header*        g_header = nullptr;
	inline proto::GuestState*    g_guest = nullptr;        // the zip-to-point target (GuestState::zipTarget)
	// `overlay marker on|off|flip|test|radius <px>`: the ring on the grapple ledge L2 + R2 launches to
	inline std::atomic<bool>     g_markerOn{ true };
	inline float                 g_markerFlip = 1.0f;     // -1 mirrors it (if Spider-Man's camera side row points right after all)
	inline bool                  g_markerTest = false;    // on Spider-Man's chest instead (checks the projection)
	inline float                 g_markerRadius = 0.0f;   // pixels; 0 = 1.4% of the frame height

	// ---- state ------------------------------------------------------------------------------------------
	inline std::atomic<ULONGLONG> g_lastDrawMs{ 0 };       // main.cpp hides Batman while this is recent
	inline std::atomic<uint64_t>  g_draws{ 0 }, g_opens{ 0 }, g_openFails{ 0 };
	inline ID3D11Device*          g_dev = nullptr;
	inline ID3D11Device1*         g_dev1 = nullptr;
	inline ID3D11DeviceContext1*  g_ctx = nullptr;
	inline ID3DDeviceContextState* g_state = nullptr;
	inline ID3D11VertexShader*    g_vs = nullptr;
	inline ID3D11PixelShader*     g_ps = nullptr;
	inline ID3D11SamplerState*    g_sampler = nullptr;
	inline ID3D11RasterizerState* g_raster = nullptr;
	inline ID3D11BlendState*      g_blend = nullptr;      // straight alpha: coverage at his edges
	inline ID3D11Buffer*          g_cb = nullptr;
	inline bool                   g_initFailed = false;
	inline IDXGISwapChain*        g_swap = nullptr;          // the swap chain the views belong to
	inline ID3D11RenderTargetView* g_rtv = nullptr;
	inline UINT                   g_bbW = 0, g_bbH = 0;
	inline std::atomic<float>        g_aspect{ 0.0f };  // Arkham's back buffer width / height (0 until the first frame)
	inline ID3D11ShaderResourceView* g_srv[proto::kCaptureSlots] = {};
	inline uint32_t               g_srvGen = 0, g_srvPid = 0;
	inline ULONGLONG              g_lastOpenTry = 0;
	inline double                 g_qpcFreq = 0.0;

	inline void ReleaseViews()
	{
		if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
		g_bbW = g_bbH = 0;
	}

	inline void ReleaseCapture()
	{
		for (auto& s : g_srv) {
			if (s) s->Release(), s = nullptr;
		}
		g_srvGen = g_srvPid = 0;
	}

	inline bool Init(IDXGISwapChain* a_swap)
	{
		if (FAILED(a_swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev)))) return false;
		if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_dev1)))) return false;
		ID3D11DeviceContext* ctx = nullptr;
		g_dev->GetImmediateContext(&ctx);
		HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx));
		ctx->Release();
		if (FAILED(hr)) return false;
		D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel();
		UINT stateFlags = (g_dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
		if (FAILED(g_dev1->CreateDeviceContextState(stateFlags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_state))) return false;
		if (FAILED(g_dev->CreateVertexShader(g_overlayVs, sizeof(g_overlayVs), nullptr, &g_vs))) return false;
		if (FAILED(g_dev->CreatePixelShader(g_overlayPs, sizeof(g_overlayPs), nullptr, &g_ps))) return false;
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(g_dev->CreateSamplerState(&sd, &g_sampler))) return false;
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		if (FAILED(g_dev->CreateRasterizerState(&rd, &g_raster))) return false;
		D3D11_BLEND_DESC bl{};
		bl.RenderTarget[0].BlendEnable = TRUE;
		bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(g_dev->CreateBlendState(&bl, &g_blend))) return false;
		D3D11_BUFFER_DESC bd{ 80, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb))) return false;
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_qpcFreq = static_cast<double>(f.QuadPart);
		Log("overlay: ready on device %p (feature level %x)", static_cast<void*>(g_dev), static_cast<unsigned>(fl));
		return true;
	}

	inline bool EnsureTarget(IDXGISwapChain* a_swap)
	{
		if (g_rtv && a_swap == g_swap) return true;
		ReleaseViews();
		ID3D11Texture2D* bb = nullptr;
		if (FAILED(a_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
		D3D11_TEXTURE2D_DESC d;
		bb->GetDesc(&d);
		D3D11_RENDER_TARGET_VIEW_DESC rv{};
		rv.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : d.Format;
		rv.ViewDimension = d.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		HRESULT hr = g_dev->CreateRenderTargetView(bb, &rv, &g_rtv);
		bb->Release();
		if (FAILED(hr)) return false;
		g_swap = a_swap;
		g_bbW = d.Width, g_bbH = d.Height;
		g_aspect = d.Height ? static_cast<float>(d.Width) / d.Height : 0.0f;
		Log("overlay: drawing into Arkham's back buffer %ux%u format %d", d.Width, d.Height, static_cast<int>(d.Format));
		return true;
	}

	// Spider-Man's textures: D3D11 ones with legacy shared handles (made on his D3D11On12 device), or
	// D3D12 ones shared by name.
	inline bool EnsureCapture(const proto::CaptureState& a_cs)
	{
		uint32_t pid = g_header ? g_header->guestPid : 0;
		if (g_srv[0] && a_cs.generation == g_srvGen && pid == g_srvPid) return true;
		ULONGLONG now = GetTickCount64();
		if (now - g_lastOpenTry < 500) return false;  // a failed open isn't retried every frame
		g_lastOpenTry = now;
		ReleaseCapture();
		for (uint32_t i = 0; i < proto::kCaptureSlots; ++i) {
			wchar_t name[64] = L"legacy handle";
			ID3D11Texture2D* tex = nullptr;
			HRESULT          hr;
			if (a_cs.share == proto::kShareLegacy) {
				hr = a_cs.handles[i] ? g_dev->OpenSharedResource(reinterpret_cast<HANDLE>(a_cs.handles[i]), __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))
									 : E_HANDLE;
			} else {
				swprintf_s(name, proto::kCaptureNameFmt, pid, a_cs.generation, i);
				hr = g_dev1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
			}
			if (SUCCEEDED(hr)) {
				hr = g_dev->CreateShaderResourceView(tex, nullptr, &g_srv[i]);
				tex->Release();
			}
			if (FAILED(hr)) {
				if (g_openFails.fetch_add(1) < 5)
					Log("overlay: can't open Spider-Man's frame %u (%ls %llx): 0x%08lx", i, name, static_cast<unsigned long long>(a_cs.handles[i]), static_cast<unsigned long>(hr));
				ReleaseCapture();
				return false;
			}
		}
		g_srvGen = a_cs.generation, g_srvPid = pid;
		g_opens.fetch_add(1);
		Log("overlay: opened Spider-Man's frames (%ux%u, generation %u of guest %u, %s)", a_cs.width, a_cs.height, a_cs.generation, pid,
			a_cs.share == proto::kShareLegacy ? "legacy handles" : "by name");
		return true;
	}

	// The zip-to-point marker: the guest's target projected with the camera of the very frame drawn (the
	// capture's), into capture uv, then into Arkham's uv the way the pixel shader maps them.
	inline void Marker(const proto::CaptureState& a_cs, float a_smTx, float a_smTy, const float a_scale[2], float a_out[4], float a_color[4])
	{
		proto::GuestState gs;
		if (!g_markerOn.load(std::memory_order_relaxed) || !g_guest || !SeqRead(g_guest, gs)) return;
		double t[3];
		if (g_markerTest) {
			for (int i = 0; i < 3; ++i) t[i] = gs.heroPos[i];
			t[1] += 0.9;  // his chest
		} else if (gs.zipFlags & proto::kZipTarget) {
			for (int i = 0; i < 3; ++i) t[i] = gs.zipTarget[i];
		} else {
			return;
		}
		double       d[3] = { t[0] - a_cs.camPos[0], t[1] - a_cs.camPos[1], t[2] - a_cs.camPos[2] };
		const float* side = a_cs.camRot;
		const float* up = a_cs.camRot + 3;
		const float* fwd = a_cs.camRot + 6;
		// rows side, up, forward are a right-handed basis (side x up = forward): side points left on screen
		double x = -(d[0] * side[0] + d[1] * side[1] + d[2] * side[2]) * g_markerFlip;
		double y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
		double z = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
		if (z < 0.3 || a_smTx <= 0.0f || a_smTy <= 0.0f) return;
		float cu = static_cast<float>(0.5 + 0.5 * x / z / a_smTx), cv = static_cast<float>(0.5 - 0.5 * y / z / a_smTy);
		float au = 0.5f + (cu - g_offset[0] - 0.5f) / a_scale[0], av = 0.5f + (cv - g_offset[1] - 0.5f) / a_scale[1];
		if (au < -0.05f || au > 1.05f || av < -0.05f || av > 1.05f) return;
		a_out[0] = au, a_out[1] = av;
		a_out[2] = g_markerRadius > 0.0f ? g_markerRadius : std::max(6.0f, g_bbH * 0.014f);
		a_out[3] = 1.0f;
		if (gs.zipFlags & proto::kZipActive) a_color[0] = 0.55f, a_color[1] = 0.8f, a_color[2] = 1.0f;  // launching: light blue
	}

	inline void Draw(IDXGISwapChain* a_swap)
	{
		if (!g_enabled.load(std::memory_order_relaxed) || !g_capture || g_initFailed || !g_puppet.load(std::memory_order_relaxed)) return;
		proto::CaptureState cs;
		if (!SeqRead(g_capture, cs) || !(cs.flags & proto::kCapLive) || cs.slot >= proto::kCaptureSlots || !cs.width || !cs.height) return;
		if (!g_dev && !Init(a_swap)) {
			g_initFailed = true;
			Log("overlay: D3D11 setup failed - no overlay");
			return;
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if ((now.QuadPart - cs.qpc) / g_qpcFreq > 0.25) return;  // Spider-Man stopped presenting
		if (!EnsureTarget(a_swap) || !EnsureCapture(cs)) return;

		// Arkham's view: horizontal FOV (UE3), this back buffer's aspect. Spider-Man's: what the guest
		// says, else the same vertical FOV as Arkham's (times `overlay scale`).
		float akFov = g_akFovDeg.load(std::memory_order_relaxed);
		if (akFov < 10.0f || akFov > 170.0f) akFov = 90.0f;
		float akTx = std::tan(akFov * 0.5f * 3.14159265f / 180.0f);
		float akTy = akTx * g_bbH / g_bbW;
		float smTx = cs.tanHalfFovX, smTy = cs.tanHalfFovY;
		if (smTx <= 0.0f || smTy <= 0.0f) {
			smTy = akTy * g_fovScale;
			smTx = smTy * cs.width / cs.height;
		}
		struct
		{
			float scale[2], offset[2], tint[4], marker[4], markerColor[4], screen[4];
		} c{ { akTx / smTx, akTy / smTy }, { g_offset[0], g_offset[1] }, { g_tint[0], g_tint[1], g_tint[2], g_gamma }, {}, { 1.0f, 1.0f, 1.0f, 0.95f },
			{ static_cast<float>(g_bbW), static_cast<float>(g_bbH), 2.5f, 0.0f } };
		Marker(cs, smTx, smTy, c.scale, c.marker, c.markerColor);

		ID3DDeviceContextState* prev = nullptr;
		g_ctx->SwapDeviceContextState(g_state, &prev);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			memcpy(m.pData, &c, sizeof(c));
			g_ctx->Unmap(g_cb, 0);
		}
		D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(g_bbW), static_cast<float>(g_bbH), 0.0f, 1.0f };
		g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_ctx->IASetInputLayout(nullptr);
		g_ctx->VSSetShader(g_vs, nullptr, 0);
		g_ctx->PSSetShader(g_ps, nullptr, 0);
		g_ctx->PSSetShaderResources(0, 1, &g_srv[cs.slot]);
		g_ctx->PSSetSamplers(0, 1, &g_sampler);
		g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
		g_ctx->RSSetState(g_raster);
		g_ctx->RSSetViewports(1, &vp);
		g_ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF);
		g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_ctx->Draw(3, 0);
		ID3D11ShaderResourceView* none = nullptr;
		g_ctx->PSSetShaderResources(0, 1, &none);
		g_ctx->SwapDeviceContextState(prev, nullptr);
		if (prev) prev->Release();
		g_lastDrawMs = GetTickCount64();
		if (g_draws.fetch_add(1) % 3600 == 0) {
			Log("overlay: Spider-Man frame %llu (slot %u, %.1f ms old) drawn; Arkham FOV %.1f, scale %.3f x %.3f", static_cast<unsigned long long>(cs.frameId), cs.slot,
				(now.QuadPart - cs.qpc) * 1000.0 / g_qpcFreq, akFov, c.scale[0], c.scale[1]);
		}
	}

	// ---- `shot [count] [every]`: Arkham's finished frames (with Spider-Man) as half-size BMPs in logs\shots ----
	inline std::atomic<int> g_shotsLeft{ 0 };
	inline int              g_shotEvery = 1, g_shotTick = 0, g_shotIndex = 0;
	inline ID3D11Texture2D* g_shotTex = nullptr;

	inline void Shot(IDXGISwapChain* a_swap)
	{
		if (!g_dev && !Init(a_swap)) return;
		ID3D11Texture2D* bb = nullptr;
		if (FAILED(a_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
		D3D11_TEXTURE2D_DESC d;
		bb->GetDesc(&d);
		bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
		bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
		if (d.SampleDesc.Count != 1 || (!bgra && !rgba)) {
			bb->Release();
			g_shotsLeft = 0;
			Log("overlay: can't take shots of a %d back buffer (%u samples)", static_cast<int>(d.Format), d.SampleDesc.Count);
			return;
		}
		D3D11_TEXTURE2D_DESC sd{};
		if (g_shotTex) g_shotTex->GetDesc(&sd);
		if (!g_shotTex || sd.Width != d.Width || sd.Height != d.Height || sd.Format != d.Format) {
			if (g_shotTex) g_shotTex->Release(), g_shotTex = nullptr;
			D3D11_TEXTURE2D_DESC td = d;
			td.Usage = D3D11_USAGE_STAGING;
			td.BindFlags = 0;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			td.MiscFlags = 0;
			if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_shotTex))) {
				bb->Release();
				return;
			}
		}
		g_ctx->CopyResource(g_shotTex, bb);
		bb->Release();
		D3D11_MAPPED_SUBRESOURCE m;
		if (FAILED(g_ctx->Map(g_shotTex, 0, D3D11_MAP_READ, 0, &m))) return;
		UINT   w = d.Width / 2, h = d.Height / 2;
		size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~static_cast<size_t>(3);  // BMP rows are padded to 4 bytes
		std::vector<uint8_t> px(stride * h);
		for (UINT y = 0; y < h; ++y) {
			const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y * 2) * m.RowPitch;
			uint8_t*       out = px.data() + static_cast<size_t>(h - 1 - y) * stride;  // BMP rows go bottom up
			for (UINT x = 0; x < w; ++x) {
				const uint8_t* p = row + x * 8;
				out[x * 3 + 0] = bgra ? p[0] : p[2];
				out[x * 3 + 1] = p[1];
				out[x * 3 + 2] = bgra ? p[2] : p[0];
			}
		}
		g_ctx->Unmap(g_shotTex, 0);
		std::wstring dir = g_logDir + L"\\shots";
		CreateDirectoryW(dir.c_str(), nullptr);
		wchar_t name[64];
		swprintf_s(name, L"\\shot_%04d.bmp", g_shotIndex++);
		if (FILE* f = _wfopen((dir + name).c_str(), L"wb")) {
			uint32_t      img = static_cast<uint32_t>(px.size());
			uint8_t       hdr[54] = { 'B', 'M' };
			uint32_t      v[13] = { 54 + img, 0, 54, 40, w, h, 1 | (24u << 16), 0, img, 2835, 2835, 0, 0 };
			memcpy(hdr + 2, v, sizeof(v));
			fwrite(hdr, 1, sizeof(hdr), f);
			fwrite(px.data(), 1, px.size(), f);
			fclose(f);
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		Log("overlay: shot %d (qpc %lld)", g_shotIndex - 1, static_cast<long long>(now.QuadPart));
	}

	inline HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* a_swap, UINT a_sync, UINT a_flags)
	{
		if (g_aspect.load(std::memory_order_relaxed) == 0.0f) {  // for the field of view (main.cpp), drawing or not
			DXGI_SWAP_CHAIN_DESC d;
			if (SUCCEEDED(a_swap->GetDesc(&d)) && d.BufferDesc.Height) g_aspect = static_cast<float>(d.BufferDesc.Width) / d.BufferDesc.Height;
		}
		if (!(a_flags & DXGI_PRESENT_TEST)) {
			Draw(a_swap);
			if (g_shotsLeft.load(std::memory_order_relaxed) > 0 && ++g_shotTick >= g_shotEvery) {
				g_shotTick = 0;
				g_shotsLeft.fetch_sub(1);
				Shot(a_swap);
			}
		}
		return g_origPresent(a_swap, a_sync, a_flags);
	}

	inline HRESULT STDMETHODCALLTYPE ResizeDetour(IDXGISwapChain* a_swap, UINT a_n, UINT a_w, UINT a_h, DXGI_FORMAT a_f, UINT a_flags)
	{
		if (a_swap == g_swap) ReleaseViews();  // a back buffer view would make the resize fail
		g_aspect = 0.0f;
		return g_origResize(a_swap, a_n, a_w, a_h, a_f, a_flags);
	}

	// From the worker thread once Arkham runs: a throwaway D3D11 swap chain on a hidden window.
	inline void Install()
	{
		auto create = reinterpret_cast<CreateDeviceAndSwapChainFn>(GetProcAddress(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain"));
		WNDCLASSW wc{ 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"ArkWebOverlayDummy" };
		RegisterClassW(&wc);
		HWND                 wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 1;
		sd.OutputWindow = wnd;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		IDXGISwapChain*      swap = nullptr;
		ID3D11Device*        dev = nullptr;
		ID3D11DeviceContext* ctx = nullptr;
		if (create && wnd && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx))) {
			g_origPresent = reinterpret_cast<PresentFn>(PatchVtableSlot(swap, kSwapPresent, reinterpret_cast<void*>(&PresentDetour)));
			g_origResize = reinterpret_cast<ResizeFn>(PatchVtableSlot(swap, kSwapResizeBuffers, reinterpret_cast<void*>(&ResizeDetour)));
		}
		Log("overlay: swap chain hooks: Present %s, ResizeBuffers %s", g_origPresent ? "ok" : "MISSING", g_origResize ? "ok" : "MISSING");
		if (ctx) ctx->Release();
		if (dev) dev->Release();
		if (swap) swap->Release();
		if (wnd) DestroyWindow(wnd);
	}

	// overlay on|off | scale <f> | offset <u> <v> | tint <r> <g> <b> | gamma <g> | status; shot [count] [every] (devcmd.h)
	inline void Command(const char* a_verb, const char* a_a, const char* a_b, const char* a_c)
	{
		if (!a_verb) a_verb = "status";
		if (!_stricmp(a_verb, "on") || !_stricmp(a_verb, "off")) {
			g_enabled = !_stricmp(a_verb, "on");
		} else if (!_stricmp(a_verb, "scale") && a_a) {
			g_fovScale = static_cast<float>(atof(a_a));
		} else if (!_stricmp(a_verb, "offset") && a_a && a_b) {
			g_offset[0] = static_cast<float>(atof(a_a)), g_offset[1] = static_cast<float>(atof(a_b));
		} else if (!_stricmp(a_verb, "gamma") && a_a) {
			g_gamma = std::max(0.2f, std::min(3.0f, static_cast<float>(atof(a_a))));
		} else if (!_stricmp(a_verb, "marker") && a_a) {
			if (!_stricmp(a_a, "on") || !_stricmp(a_a, "off")) g_markerOn = !_stricmp(a_a, "on");
			if (!_stricmp(a_a, "flip")) g_markerFlip = -g_markerFlip;
			if (!_stricmp(a_a, "test")) g_markerTest = !g_markerTest;
			if (!_stricmp(a_a, "radius") && a_b) g_markerRadius = static_cast<float>(atof(a_b));
			Log("  zip marker %s%s%s, radius %.0f", g_markerOn ? "on" : "off", g_markerFlip < 0 ? ", mirrored" : "", g_markerTest ? ", TEST (on Spider-Man)" : "",
				g_markerRadius);
		} else if (!_stricmp(a_verb, "tint") && a_a && a_b && a_c) {
			g_tint[0] = static_cast<float>(atof(a_a)), g_tint[1] = static_cast<float>(atof(a_b)), g_tint[2] = static_cast<float>(atof(a_c));
		}
		proto::CaptureState cs{};
		bool                live = g_capture && SeqRead(g_capture, cs) && (cs.flags & proto::kCapLive);
		Log("  overlay %s: hooks %s, %llu frames drawn, opened %llu times (%llu failures), Spider-Man frames %s (%ux%u gen %u, tan %.3f %.3f), "
			"Arkham FOV %.1f, scale %.3f, offset %.3f %.3f, tint %.2f %.2f %.2f, gamma %.2f",
			g_enabled ? "ON" : "off", g_origPresent ? "ok" : "missing", static_cast<unsigned long long>(g_draws.load()), static_cast<unsigned long long>(g_opens.load()),
			static_cast<unsigned long long>(g_openFails.load()), live ? "live" : "not coming", cs.width, cs.height, cs.generation, cs.tanHalfFovX, cs.tanHalfFovY,
			g_akFovDeg.load(), g_fovScale, g_offset[0], g_offset[1], g_tint[0], g_tint[1], g_tint[2], g_gamma);
	}
}
