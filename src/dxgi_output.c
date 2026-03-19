/*
 * dxgi_output.c — DXGI 1.2 stereo swap chain output for NVIDIA 3D Vision
 *
 * All D3D11/DXGI COM interfaces are accessed through raw vtable indices so
 * that d3d11.h / dxgi.h are NOT required at compile time.  The DLLs are
 * loaded dynamically at runtime.
 *
 * External-memory architecture (no CPU staging)
 * ─────────────────────────────────────────────
 * Instead of copying pixels CPU-side, the Vulkan render target IS the
 * D3D11 intermediate texture.  Shared NT handles let both APIs refer to
 * the same physical GPU allocation:
 *
 *   1.  dxgi_shared_tex_create:
 *         D3D11 CreateTexture2D(ArraySize=2, SHARED_NTHANDLE) → NT HANDLE
 *   2.  swapchain.c:
 *         VkImage created with VkExternalMemoryImageCreateInfo +
 *         VkImportMemoryWin32HandleInfoKHR → same physical pages as above
 *   3.  App renders into VkImage (multiview: slice 0=left, slice 1=right)
 *   4.  present.c:  WaitForFences (render complete)
 *   5.  dxgi_copy_and_present:
 *         CopySubresourceRegion  shared_tex[0] → DXGI back-buf[0]  (GPU)
 *         CopySubresourceRegion  shared_tex[1] → DXGI back-buf[1]  (GPU)
 *         IDXGISwapChain::Present(1, 0)
 *
 * Zero PCIe round-trip.  ~15-25% overhead vs mono at GPU.
 *
 * NVAPI is used only for NvAPI_Stereo_Enable / _Activate (3D Vision hint).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"

/* ── D3D11 / DXGI constants (no SDK headers needed) ─────────────────────── */
#define D3D_DRIVER_TYPE_HARDWARE        1
#define D3D_FEATURE_LEVEL_11_0          0xb000
#define DXGI_FORMAT_UNKNOWN             0
#define DXGI_FORMAT_R8G8B8A8_UNORM      28
#define DXGI_FORMAT_R8G8B8A8_UNORM_SRGB 29
#define DXGI_FORMAT_B8G8R8A8_UNORM      87
#define DXGI_FORMAT_B8G8R8A8_UNORM_SRGB 91
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x20u
#define DXGI_SWAP_EFFECT_FLIP_DISCARD   4
#define DXGI_SCALING_NONE               1
#define DXGI_ALPHA_MODE_UNSPECIFIED     0
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH 2u
#define D3D11_USAGE_DEFAULT             0
#define D3D11_BIND_SHADER_RESOURCE      0x8u
#define D3D11_BIND_RENDER_TARGET        0x20u
/* D3D11_RESOURCE_MISC_SHARED_NTHANDLE: enable NT-handle sharing (Win8+).
 * MSDN value: 0x800.  (Previously was incorrectly defined as 0x800000 which
 * caused E_INVALIDARG from CreateTexture2D on all driver versions.) */
#define D3D11_RESOURCE_MISC_SHARED_NTHANDLE 0x800u

/* ── COM helper macros ───────────────────────────────────────────────────── */
#define COM_HR(o,N,...) \
    ((HRESULT(WINAPI*)(void*,##__VA_ARGS__))(*(void***)(o))[N])((o),##__VA_ARGS__)
#define COM_VOID(o,N,...) \
    ((void(WINAPI*)(void*,##__VA_ARGS__))(*(void***)(o))[N])((o),##__VA_ARGS__)
#define COM_RELEASE(o) do { if(o){ COM_HR((o),2); (o)=NULL; } } while(0)

/* ── IIDs ────────────────────────────────────────────────────────────────── */
static const GUID IID_IDXGIDevice   = {0x54EC77FA,0x1377,0x44E6,{0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C}};
static const GUID IID_IDXGIFactory2 = {0x50C83A1C,0xE072,0x4C48,{0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0}};
static const GUID IID_ID3D11Tex2D   = {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};
/* IDXGIResource1 — for CreateSharedHandle (NT handle) */
static const GUID IID_IDXGIResource1 = {0x30961379,0x4609,0x4A41,{0x99,0x8E,0x54,0xFE,0x56,0x7E,0xE0,0xC1}};

/* ── Struct layouts (no SDK needed) ─────────────────────────────────────── */
typedef struct { UINT N, D; } DXGI_RATIONAL_;
typedef struct { UINT Count, Quality; } DXGI_SAMPLE_DESC_;

typedef struct {
    UINT Width, Height;
    UINT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC_ SampleDesc;
    UINT BufferUsage;
    UINT BufferCount;
    UINT Scaling;
    UINT SwapEffect;
    UINT AlphaMode;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC1_;

typedef struct {
    DXGI_RATIONAL_ RefreshRate;
    UINT           ScanlineOrdering;
    UINT           Rotation;
    BOOL           Windowed;
} DXGI_SC_FULLSCREEN_DESC_;

typedef struct {
    UINT Width, Height, MipLevels, ArraySize;
    UINT Format;
    DXGI_SAMPLE_DESC_ SampleDesc;
    UINT Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
} D3D11_TEXTURE2D_DESC_;

/* ── vtable index reference ──────────────────────────────────────────────── *
 * IUnknown:         0=QI  1=AddRef  2=Release
 * IDXGIObject:      3..6
 * IDXGIDevice:      7=GetAdapter  (IDXGIObject base: 3..6)
 * IDXGIAdapter:     7=EnumOutputs  GetParent=6
 * IDXGIFactory1:    7=EnumAdapters ... 13=IsCurrent
 * IDXGIFactory2:    14=IsWindowedStereoEnabled  15=CreateSwapChainForHwnd
 * IDXGISwapChain:   8=Present  9=GetBuffer  10=SetFullscreenState
 * IDXGIResource:    8=GetSharedHandle .. 11=GetEvictionPriority
 * IDXGIResource1:   12=CreateSharedHandle
 * ID3D11Device:     5=CreateTexture2D  40=GetImmediateContext
 * ID3D11DeviceContext: 46=CopySubresourceRegion  47=CopyResource
 * ─────────────────────────────────────────────────────────────────────────── */

typedef HRESULT (WINAPI *PFN_D3D11CD)(
    void*, int, HMODULE, UINT, const int*, UINT, UINT,
    void**, int*, void**);

/* ── NVAPI ───────────────────────────────────────────────────────────────── */
typedef void* (*PFN_NvQI)(unsigned id);
typedef int   (*PFN_NvVoid)(void);
typedef int   (*PFN_NvFromIUnk)(void*, void**);
typedef int   (*PFN_NvHandle)(void*);
typedef int   (*PFN_NvSetF)(void*, float);

#define NvID_Initialize    0x0150E828u
#define NvID_Unload        0xD22BDD7Eu
#define NvID_StereoEnable  0x239C4545u
#define NvID_StereoCreate  0xAC7E37F4u
#define NvID_StereoDestroy 0x3A153134u
#define NvID_StereoActivate 0xF6A1AD68u
#define NvID_StereoSetSep  0x5C069FA3u

static PFN_NvQI s_nvQI = NULL;
static void* nv_query(unsigned id) { return s_nvQI ? s_nvQI(id) : NULL; }

/* ── Map VkFormat → DXGI_FORMAT ─────────────────────────────────────────── */
static UINT vkfmt_to_dxgi(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_B8G8R8A8_UNORM:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:   return DXGI_FORMAT_B8G8R8A8_UNORM; /* canonical UNORM */
    case VK_FORMAT_R8G8B8A8_UNORM:  return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:                         return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

/* ── Initialize D3D11 device + NVAPI (call once per StereoDevice) ─────── */
bool dxgi_device_init(StereoDevice *sd)
{
    if (sd->d3d11_ok) return true;

    HMODULE hNvapi = LoadLibraryA("nvapi64.dll");
    if (!hNvapi) hNvapi = LoadLibraryA("nvapi.dll");
    if (hNvapi) {
        s_nvQI = (PFN_NvQI)GetProcAddress(hNvapi, "nvapi_QueryInterface");
        if (s_nvQI) {
            PFN_NvVoid fnInit = (PFN_NvVoid)nv_query(NvID_Initialize);
            if (fnInit && fnInit() == 0) {
                PFN_NvVoid fnEnable = (PFN_NvVoid)nv_query(NvID_StereoEnable);
                if (fnEnable) fnEnable();
                STEREO_LOG("[DXGI] NVAPI: Stereo_Enable called");
            }
        }
        sd->nvapi_lib = hNvapi;
    }

    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) { STEREO_ERR("[DXGI] d3d11.dll not found"); return false; }

    PFN_D3D11CD fnCD = (PFN_D3D11CD)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!fnCD) { FreeLibrary(hD3D11); STEREO_ERR("[DXGI] D3D11CreateDevice not found"); return false; }

    int fl = D3D_FEATURE_LEVEL_11_0, flOut = 0;
    void *pDev = NULL, *pCtx = NULL;
    HRESULT hr = fnCD(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                      &fl, 1, 7, &pDev, &flOut, &pCtx);
    if (FAILED(hr) || !pDev) {
        STEREO_ERR("[DXGI] D3D11CreateDevice failed: 0x%x", (unsigned)hr);
        FreeLibrary(hD3D11);
        return false;
    }
    STEREO_LOG("[DXGI] D3D11 device OK  dev=%p ctx=%p", pDev, pCtx);

    sd->d3d11_dev = pDev;
    sd->d3d11_ctx = pCtx;
    sd->d3d11_lib = hD3D11;
    sd->d3d11_ok  = true;

    if (s_nvQI) {
        PFN_NvFromIUnk fnCr = (PFN_NvFromIUnk)nv_query(NvID_StereoCreate);
        if (fnCr) {
            void *hStereo = NULL;
            int r = fnCr(pDev, &hStereo);
            STEREO_LOG("[DXGI] NvAPI_Stereo_CreateHandleFromIUnknown = %d  handle=%p", r, hStereo);
            if (r == 0) {
                sd->nvapi_stereo = hStereo;
                PFN_NvHandle fnAct = (PFN_NvHandle)nv_query(NvID_StereoActivate);
                if (fnAct) { r = fnAct(hStereo); STEREO_LOG("[DXGI] NvAPI_Stereo_Activate = %d", r); }
                PFN_NvSetF fnSep = (PFN_NvSetF)nv_query(NvID_StereoSetSep);
                if (fnSep) fnSep(hStereo, 50.f);
            }
        }
    }
    return true;
}

/* ── Create DXGI 1.2 stereo swap chain on sc->hwnd ───────────────────── */
bool dxgi_sc_create(StereoDevice *sd, StereoSwapchain *sc)
{
    if (!sd->d3d11_ok || !sc->hwnd) return false;

    void *pDev = sd->d3d11_dev;

    void *pDXGIDev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, const GUID*, void**))(*(void***)pDev)[0])
                 (pDev, &IID_IDXGIDevice, &pDXGIDev);
    if (FAILED(hr) || !pDXGIDev) { STEREO_ERR("[DXGI] QI IDXGIDevice failed: 0x%x", (unsigned)hr); return false; }

    void *pAdapter = NULL;
    hr = ((HRESULT(WINAPI*)(void*, void**))(*(void***)pDXGIDev)[7])(pDXGIDev, &pAdapter);
    COM_RELEASE(pDXGIDev);
    if (FAILED(hr) || !pAdapter) { STEREO_ERR("[DXGI] GetAdapter failed: 0x%x", (unsigned)hr); return false; }

    void *pFact2 = NULL;
    hr = ((HRESULT(WINAPI*)(void*, const GUID*, void**))(*(void***)pAdapter)[6])
         (pAdapter, &IID_IDXGIFactory2, &pFact2);
    COM_RELEASE(pAdapter);
    if (FAILED(hr) || !pFact2) { STEREO_ERR("[DXGI] GetParent IDXGIFactory2 failed: 0x%x", (unsigned)hr); return false; }

    BOOL wse = ((BOOL(WINAPI*)(void*))(*(void***)pFact2)[14])(pFact2);
    STEREO_LOG("[DXGI] IsWindowedStereoEnabled = %s", wse ? "TRUE" : "FALSE");

    DXGI_SWAP_CHAIN_DESC1_ scd = {
        .Width       = sc->app_width,
        .Height      = sc->app_height,
        .Format      = DXGI_FORMAT_B8G8R8A8_UNORM,
        .Stereo      = TRUE,
        .SampleDesc  = {1, 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 2,
        .Scaling     = DXGI_SCALING_NONE,
        .SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH,
    };
    DXGI_SC_FULLSCREEN_DESC_ fsd = { .RefreshRate = {120, 1}, .Windowed = FALSE };

    void *pSC = NULL;
    hr = ((HRESULT(WINAPI*)(void*, void*, HWND,
                             const DXGI_SWAP_CHAIN_DESC1_*,
                             const DXGI_SC_FULLSCREEN_DESC_*,
                             void*, void**))(*(void***)pFact2)[15])
         (pFact2, pDev, sc->hwnd, &scd, &fsd, NULL, &pSC);
    STEREO_LOG("[DXGI] CreateSwapChainForHwnd(Stereo=TRUE, FSE): hr=0x%x  sc=%p", (unsigned)hr, pSC);

    if (FAILED(hr) || !pSC) {
        STEREO_LOG("[DXGI] FSE failed — retrying windowed");
        hr = ((HRESULT(WINAPI*)(void*, void*, HWND,
                                 const DXGI_SWAP_CHAIN_DESC1_*,
                                 const DXGI_SC_FULLSCREEN_DESC_*,
                                 void*, void**))(*(void***)pFact2)[15])
             (pFact2, pDev, sc->hwnd, &scd, NULL, NULL, &pSC);
        STEREO_LOG("[DXGI] windowed retry: hr=0x%x  sc=%p", (unsigned)hr, pSC);
    }

    COM_RELEASE(pFact2);
    if (FAILED(hr) || !pSC) { STEREO_ERR("[DXGI] Failed to create stereo swap chain: 0x%x", (unsigned)hr); return false; }

    sc->dxgi_sc = pSC;
    STEREO_LOG("[DXGI] Stereo swap chain created: %p  size=%ux%u", pSC, sc->app_width, sc->app_height);
    return true;
}

/* ── Create D3D11 Texture2DArray[2] with SHARED_NTHANDLE ─────────────── *
 *
 * This texture is the shared buffer between D3D11 and Vulkan.  It is NOT
 * the DXGI swap-chain back buffer; it is an intermediate that Vulkan renders
 * into directly (via external-memory import) and D3D11 copies to the DXGI
 * back buffer each frame.
 *
 * Stores ID3D11Texture2D* in sc->shared_d3d11_tex.
 * Returns the NT HANDLE in *out_nt_handle — ownership passes to Vulkan on
 * the first successful vkAllocateMemory call; callers must NOT CloseHandle.
 * ─────────────────────────────────────────────────────────────────────────── */
bool dxgi_shared_tex_create(StereoDevice *sd, StereoSwapchain *sc, HANDLE *out_nt_handle)
{
    UINT dxgi_fmt = vkfmt_to_dxgi(sc->format);

    D3D11_TEXTURE2D_DESC_ desc = {
        .Width          = sc->app_width,
        .Height         = sc->app_height,
        .MipLevels      = 1,
        .ArraySize      = 2,   /* slice 0 = left eye, slice 1 = right eye */
        .Format         = dxgi_fmt,
        .SampleDesc     = {1, 0},
        .Usage          = D3D11_USAGE_DEFAULT,
        .BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        .CPUAccessFlags = 0,
        .MiscFlags      = D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };

    void *pTex = NULL;
    /* ID3D11Device::CreateTexture2D = vtable[5] */
    HRESULT hr = ((HRESULT(WINAPI*)(void*, const D3D11_TEXTURE2D_DESC_*, void*, void**))
                  (*(void***)sd->d3d11_dev)[5])
                 (sd->d3d11_dev, &desc, NULL, &pTex);
    if (FAILED(hr) || !pTex) {
        STEREO_ERR("[DXGI] CreateTexture2D(shared stereo) failed: 0x%x", (unsigned)hr);
        return false;
    }

    /* QI for IDXGIResource1 to call CreateSharedHandle (NT handles) */
    void *pRes1 = NULL;
    hr = ((HRESULT(WINAPI*)(void*, const GUID*, void**))(*(void***)pTex)[0])
         (pTex, &IID_IDXGIResource1, &pRes1);
    if (FAILED(hr) || !pRes1) {
        STEREO_ERR("[DXGI] QI IDXGIResource1 failed: 0x%x (Win8+ required)", (unsigned)hr);
        COM_RELEASE(pTex);
        return false;
    }

    /* IDXGIResource1::CreateSharedHandle = vtable[12]
     * Access GENERIC_ALL (0x10000000) — Vulkan imports with full access */
    HANDLE hShared = NULL;
    hr = ((HRESULT(WINAPI*)(void*, void*, DWORD, LPCWSTR, HANDLE*))
          (*(void***)pRes1)[12])
         (pRes1, NULL, 0x10000000u /*GENERIC_ALL*/, NULL, &hShared);
    COM_RELEASE(pRes1);
    if (FAILED(hr) || !hShared) {
        STEREO_ERR("[DXGI] CreateSharedHandle failed: 0x%x", (unsigned)hr);
        COM_RELEASE(pTex);
        return false;
    }

    sc->shared_d3d11_tex = pTex;
    sc->shared_nt_handle = hShared;
    *out_nt_handle       = hShared;
    STEREO_LOG("[DXGI] Shared stereo texture created: tex=%p  fmt=%u  handle=%p",
               pTex, dxgi_fmt, hShared);
    return true;
}

/* ── GPU copy from shared texture → DXGI back buffer + Present ──────── *
 *
 * Called after CPU WaitForFences (Vulkan render complete).
 * D3D11 submits two GPU-side CopySubresourceRegion commands then presents.
 * No CPU pixel data is read or written.
 * ─────────────────────────────────────────────────────────────────────────── */
VkResult dxgi_copy_and_present(StereoDevice *sd, StereoSwapchain *sc)
{
    void *pCtx = sd->d3d11_ctx;

    /* Acquire DXGI back buffer (Texture2DArray[2]) */
    void *pBB = NULL;
    /* IDXGISwapChain::GetBuffer = vtable[9] */
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, const GUID*, void**))
                  (*(void***)sc->dxgi_sc)[9])
                 (sc->dxgi_sc, 0, &IID_ID3D11Tex2D, &pBB);
    if (FAILED(hr) || !pBB) {
        STEREO_ERR("[DXGI] GetBuffer failed: 0x%x", (unsigned)hr);
        return VK_ERROR_DEVICE_LOST;
    }

    /* CopySubresourceRegion: shared_tex slice 0 → back buffer slice 0 (left)
     * vtable[46] = CopySubresourceRegion(dst, DstSub, DstX, DstY, DstZ, src, SrcSub, pBox) */
    ((void(WINAPI*)(void*, void*, UINT, UINT, UINT, UINT, void*, UINT, void*))
     (*(void***)pCtx)[46])
    (pCtx, pBB, 0, 0, 0, 0, sc->shared_d3d11_tex, 0, NULL);

    /* CopySubresourceRegion: shared_tex slice 1 → back buffer slice 1 (right) */
    ((void(WINAPI*)(void*, void*, UINT, UINT, UINT, UINT, void*, UINT, void*))
     (*(void***)pCtx)[46])
    (pCtx, pBB, 1, 0, 0, 0, sc->shared_d3d11_tex, 1, NULL);

    COM_RELEASE(pBB);

    /* IDXGISwapChain::Present = vtable[8]
     * Sync interval 0 (no vsync): 3D Vision driver enforces its own 120 Hz
     * pacing; blocking for DXGI vsync serializes the pipeline and caps at ~60. */
    hr = ((HRESULT(WINAPI*)(void*, UINT, UINT))(*(void***)sc->dxgi_sc)[8])
         (sc->dxgi_sc, 0, 0);

    if (hr == 0x087A0001 /* DXGI_STATUS_OCCLUDED */) {
        STEREO_LOG("[DXGI] Present: OCCLUDED (window not focused)");
        return VK_SUCCESS;
    }
    if (FAILED(hr)) {
        STEREO_ERR("[DXGI] Present failed: 0x%x", (unsigned)hr);
        return VK_ERROR_DEVICE_LOST;
    }

    /* Re-activate stereo each frame (some drivers deactivate on Alt+Tab) */
    if (sd->nvapi_stereo) {
        PFN_NvHandle fnAct = (PFN_NvHandle)nv_query(NvID_StereoActivate);
        if (fnAct) fnAct(sd->nvapi_stereo);
    }
    return VK_SUCCESS;
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */
void dxgi_sc_destroy(StereoSwapchain *sc)
{
    COM_RELEASE(sc->shared_d3d11_tex);
    /* shared_nt_handle ownership was consumed by Vulkan import; do not CloseHandle */
    sc->shared_nt_handle = NULL;
    COM_RELEASE(sc->dxgi_sc);
}

void dxgi_device_destroy(StereoDevice *sd)
{
    if (sd->nvapi_stereo && s_nvQI) {
        PFN_NvHandle fnDest = (PFN_NvHandle)nv_query(NvID_StereoDestroy);
        if (fnDest) fnDest(sd->nvapi_stereo);
        sd->nvapi_stereo = NULL;
        PFN_NvVoid fnUn = (PFN_NvVoid)nv_query(NvID_Unload);
        if (fnUn) fnUn();
    }
    if (sd->nvapi_lib) { FreeLibrary((HMODULE)sd->nvapi_lib); sd->nvapi_lib = NULL; }
    COM_RELEASE(sd->d3d11_ctx);
    COM_RELEASE(sd->d3d11_dev);
    if (sd->d3d11_lib) { FreeLibrary((HMODULE)sd->d3d11_lib); sd->d3d11_lib = NULL; }
    sd->d3d11_ok = false;
}
