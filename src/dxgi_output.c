/*
 * dxgi_output.c — DXGI 1.2 stereo swap chain output for NVIDIA 3D Vision
 *
 * All D3D11/DXGI COM interfaces are accessed through raw vtable indices so
 * that d3d11.h / dxgi.h are NOT required at compile time.  The DLLs are
 * loaded dynamically at runtime.
 *
 * Flow per frame
 * ──────────────
 * 1. App renders into stereo_images[idx] (VkImage, arrayLayers=2,
 *    layer 0 = left eye, layer 1 = right eye) via Vulkan multiview.
 * 2. App calls QueuePresentKHR → VKS3D intercepts.
 * 3. VKS3D submits a staging command buffer that:
 *      a. Transitions stereo_image: COLOR_ATTACHMENT → TRANSFER_SRC
 *      b. vkCmdCopyImageToBuffer: layer 0 → stage_buf[0..W*H*4)
 *                                 layer 1 → stage_buf[W*H*4..2*W*H*4)
 *      c. Transitions back: TRANSFER_SRC → COLOR_ATTACHMENT
 *    This CB waits on the app's render semaphores before starting.
 * 4. CPU waits for staging fence.
 * 5. D3D11 UpdateSubresource: staged pixels → left_tex / right_tex
 * 6. IDXGISwapChain::GetBuffer (0) → back-buffer Texture2DArray[2]
 * 7. D3D11 CopySubresourceRegion: left_tex → bb slice 0
 *                                  right_tex → bb slice 1
 * 8. IDXGISwapChain::Present(1, 0)
 *
 * NVAPI is used only for NvAPI_Stereo_Enable (pre-device hint) and
 * NvAPI_Stereo_CreateHandleFromIUnknown / Activate (3D Vision activation).
 * Per-eye rendering goes through DXGI, not NVAPI.
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
#define D3D11_RESOURCE_MISC_SHARED      0x2u

/* ── COM helper macros ───────────────────────────────────────────────────── */
/* Call vtable slot N on COM object o, returning HRESULT */
#define COM_HR(o,N,...) \
    ((HRESULT(WINAPI*)(void*,##__VA_ARGS__))(*(void***)(o))[N])((o),##__VA_ARGS__)
/* Call vtable slot N returning void */
#define COM_VOID(o,N,...) \
    ((void(WINAPI*)(void*,##__VA_ARGS__))(*(void***)(o))[N])((o),##__VA_ARGS__)
/* Release a COM object (IUnknown::Release = vtable[2]) */
#define COM_RELEASE(o) do { if(o){ COM_HR((o),2); (o)=NULL; } } while(0)

/* ── IIDs ────────────────────────────────────────────────────────────────── */
static const GUID IID_IDXGIDevice  = {0x54EC77FA,0x1377,0x44E6,{0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C}};
static const GUID IID_IDXGIFactory2= {0x50C83A1C,0xE072,0x4C48,{0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0}};
static const GUID IID_ID3D11Tex2D  = {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};

/* ── Struct layouts (no SDK needed) ─────────────────────────────────────── */
typedef struct { UINT N, D; } DXGI_RATIONAL_;
typedef struct { UINT Count, Quality; } DXGI_SAMPLE_DESC_;

typedef struct {
    UINT Width, Height;
    UINT Format;        /* DXGI_FORMAT */
    BOOL Stereo;
    DXGI_SAMPLE_DESC_ SampleDesc;
    UINT BufferUsage;
    UINT BufferCount;
    UINT Scaling;       /* DXGI_SCALING */
    UINT SwapEffect;    /* DXGI_SWAP_EFFECT */
    UINT AlphaMode;     /* DXGI_ALPHA_MODE */
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
    UINT Format;        /* DXGI_FORMAT */
    DXGI_SAMPLE_DESC_ SampleDesc;
    UINT Usage;         /* D3D11_USAGE */
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
} D3D11_TEXTURE2D_DESC_;

/* ── vtable index reference ──────────────────────────────────────────────── *
 *
 * IUnknown:                  0=QI  1=AddRef  2=Release
 * IDXGIObject:               3=SetPrivateData  4=SetPrivateDataInterface
 *                            5=GetPrivateData  6=GetParent
 * IDXGIDeviceSubObject:      7=GetDevice
 * IDXGIDevice:               8=GetAdapter (first IDXGIDevice-specific method)
 *   (IDXGIDevice inherits IDXGIObject, not IDXGIDeviceSubObject)
 *   3=SetPrivateData..6=GetParent, then 7=GetAdapter (IDXGIDevice specific)
 *
 * IDXGIAdapter inherits IDXGIObject: 7=EnumOutputs, 8=GetDesc, 9=CheckInterfaceSupport
 *   GetParent = vtable[6]
 *
 * IDXGIFactory  (inherits IDXGIObject): 7=EnumAdapters, 8=MakeWindowAssociation,
 *               9=GetWindowAssociation, 10=CreateSwapChain, 11=CreateSoftwareAdapter
 * IDXGIFactory1 (inherits IDXGIFactory): 12=EnumAdapters1, 13=IsCurrent
 * IDXGIFactory2 (inherits IDXGIFactory1): 14=IsWindowedStereoEnabled,
 *               15=CreateSwapChainForHwnd
 *
 * IDXGISwapChain (inherits IDXGIDeviceSubObject=7, so sc methods start at 8):
 *   8=Present, 9=GetBuffer, 10=SetFullscreenState, 11=GetFullscreenState,
 *   12=GetDesc, 13=ResizeBuffers
 *
 * IDXGIResource (inherits IDXGIDeviceSubObject=7):
 *   8=GetSharedHandle, 9=GetUsage, 10=SetEvictionPriority, 11=GetEvictionPriority
 *
 * ID3D11Device  (inherits IUnknown):
 *   3=CreateBuffer, 4=CreateTexture1D, 5=CreateTexture2D, 6=CreateTexture3D,
 *   7=CreateShaderResourceView, 8=CreateUnorderedAccessView,
 *   9=CreateRenderTargetView, 40=GetImmediateContext
 *
 * ID3D11DeviceContext (inherits ID3D11DeviceChild:3..6, IUnknown:0..2):
 *   33=OMSetRenderTargets, 46=CopySubresourceRegion, 47=CopyResource,
 *   48=UpdateSubresource, 50=ClearRenderTargetView
 *
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── D3D11 device creation function type ────────────────────────────────── */
typedef HRESULT (WINAPI *PFN_D3D11CD)(
    void *pAdapter, int DriverType, HMODULE Software, UINT Flags,
    const int *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext);

/* ── NVAPI function types / IDs ─────────────────────────────────────────── */
typedef void* (*PFN_NvQI)(unsigned id);
typedef int   (*PFN_NvVoid)(void);
typedef int   (*PFN_NvFromIUnk)(void *pDev, void **ppHandle);
typedef int   (*PFN_NvHandle)(void *hStereo);
typedef int   (*PFN_NvSetF)(void *hStereo, float v);

#define NvID_Initialize    0x0150E828u
#define NvID_Unload        0xD22BDD7Eu
#define NvID_StereoEnable  0x239C4545u
#define NvID_StereoCreate  0xAC7E37F4u
#define NvID_StereoDestroy 0x3A153134u
#define NvID_StereoActivate 0xF6A1AD68u
#define NvID_StereoSetSep  0x5C069FA3u

static PFN_NvQI s_nvQI = NULL;

static void* nv_query(unsigned id)
{
    return s_nvQI ? s_nvQI(id) : NULL;
}

/* ── Map VkFormat → DXGI_FORMAT ─────────────────────────────────────────── */
static UINT vkfmt_to_dxgi(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_B8G8R8A8_UNORM:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:   return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:  return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

/* ── Initialize D3D11 device + NVAPI (call once per StereoDevice) ─────── */
bool dxgi_device_init(StereoDevice *sd)
{
    if (sd->d3d11_ok) return true;

    /* ── NVAPI: optional, failure is non-fatal ──────────────────────── */
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
    } else {
        STEREO_LOG("[DXGI] nvapi64.dll not found — 3D Vision activation skipped");
    }

    /* ── D3D11 device ────────────────────────────────────────────────── */
    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) {
        STEREO_ERR("[DXGI] d3d11.dll not found");
        return false;
    }
    PFN_D3D11CD fnCD = (PFN_D3D11CD)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!fnCD) {
        STEREO_ERR("[DXGI] D3D11CreateDevice not found");
        FreeLibrary(hD3D11);
        return false;
    }

    int fl = D3D_FEATURE_LEVEL_11_0, flOut = 0;
    void *pDev = NULL, *pCtx = NULL;
    HRESULT hr = fnCD(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                      &fl, 1, 7 /* D3D11_SDK_VERSION */,
                      &pDev, &flOut, &pCtx);
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

    /* Create NVAPI stereo handle from D3D11 device */
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

    /* QI chain: ID3D11Device → IDXGIDevice → GetAdapter → GetParent(IDXGIFactory2) */
    void *pDXGIDev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, const GUID*, void**))(*(void***)pDev)[0])
         (pDev, &IID_IDXGIDevice, &pDXGIDev);
    if (FAILED(hr) || !pDXGIDev) {
        STEREO_ERR("[DXGI] QI IDXGIDevice failed: 0x%x", (unsigned)hr);
        return false;
    }

    /* IDXGIDevice::GetAdapter = vtable[7] */
    void *pAdapter = NULL;
    hr = ((HRESULT(WINAPI*)(void*, void**))(*(void***)pDXGIDev)[7])(pDXGIDev, &pAdapter);
    COM_RELEASE(pDXGIDev);
    if (FAILED(hr) || !pAdapter) {
        STEREO_ERR("[DXGI] GetAdapter failed: 0x%x", (unsigned)hr);
        return false;
    }

    /* IDXGIAdapter::GetParent(IDXGIFactory2) = vtable[6] */
    void *pFact2 = NULL;
    hr = ((HRESULT(WINAPI*)(void*, const GUID*, void**))(*(void***)pAdapter)[6])
         (pAdapter, &IID_IDXGIFactory2, &pFact2);
    COM_RELEASE(pAdapter);
    if (FAILED(hr) || !pFact2) {
        STEREO_ERR("[DXGI] GetParent IDXGIFactory2 failed: 0x%x", (unsigned)hr);
        return false;
    }

    /* IDXGIFactory2::IsWindowedStereoEnabled = vtable[14] */
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

    /* Request exclusive fullscreen at creation (required for 3D Vision stereo) */
    DXGI_SC_FULLSCREEN_DESC_ fsd = {
        .RefreshRate = {120, 1},
        .Windowed    = FALSE,
    };

    void *pSC = NULL;
    /* IDXGIFactory2::CreateSwapChainForHwnd = vtable[15] */
    hr = ((HRESULT(WINAPI*)(void*, void*, HWND, const DXGI_SWAP_CHAIN_DESC1_*,
                             const DXGI_SC_FULLSCREEN_DESC_*, void*, void**))(*(void***)pFact2)[15])
         (pFact2, pDev, sc->hwnd, &scd, &fsd, NULL, &pSC);
    STEREO_LOG("[DXGI] CreateSwapChainForHwnd(Stereo=TRUE, FSE): hr=0x%x  sc=%p",
               (unsigned)hr, pSC);

    if (FAILED(hr) || !pSC) {
        /* Fallback: try windowed */
        STEREO_LOG("[DXGI] FSE failed — retrying windowed");
        hr = ((HRESULT(WINAPI*)(void*, void*, HWND, const DXGI_SWAP_CHAIN_DESC1_*,
                                 const DXGI_SC_FULLSCREEN_DESC_*, void*, void**))(*(void***)pFact2)[15])
             (pFact2, pDev, sc->hwnd, &scd, NULL, NULL, &pSC);
        STEREO_LOG("[DXGI] windowed retry: hr=0x%x  sc=%p", (unsigned)hr, pSC);
    }

    COM_RELEASE(pFact2);
    if (FAILED(hr) || !pSC) {
        STEREO_ERR("[DXGI] Failed to create stereo swap chain: 0x%x", (unsigned)hr);
        return false;
    }

    sc->dxgi_sc = pSC;
    STEREO_LOG("[DXGI] Stereo swap chain created: %p  size=%ux%u",
               pSC, sc->app_width, sc->app_height);
    return true;
}

/* ── Create per-swapchain D3D11 staging textures (left + right eye) ───── */
bool dxgi_tex_create(StereoDevice *sd, StereoSwapchain *sc)
{
    UINT dxgi_fmt = vkfmt_to_dxgi(sc->format);
    /* Use UNORM for UpdateSubresource (sRGB correction via swap-chain gamma if needed) */
    if (dxgi_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) dxgi_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (dxgi_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) dxgi_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D11_TEXTURE2D_DESC_ desc = {
        .Width      = sc->app_width,
        .Height     = sc->app_height,
        .MipLevels  = 1,
        .ArraySize  = 1,
        .Format     = dxgi_fmt,
        .SampleDesc = {1, 0},
        .Usage      = D3D11_USAGE_DEFAULT,
        .BindFlags  = D3D11_BIND_SHADER_RESOURCE,
        .MiscFlags  = 0,
    };

    HRESULT hr;
    /* ID3D11Device::CreateTexture2D = vtable[5] */
    void *pLeft = NULL, *pRight = NULL;
    hr = ((HRESULT(WINAPI*)(void*, const D3D11_TEXTURE2D_DESC_*, void*, void**))(*(void***)sd->d3d11_dev)[5])
         (sd->d3d11_dev, &desc, NULL, &pLeft);
    if (FAILED(hr)) { STEREO_ERR("[DXGI] CreateTexture2D(left) failed: 0x%x", (unsigned)hr); return false; }
    hr = ((HRESULT(WINAPI*)(void*, const D3D11_TEXTURE2D_DESC_*, void*, void**))(*(void***)sd->d3d11_dev)[5])
         (sd->d3d11_dev, &desc, NULL, &pRight);
    if (FAILED(hr)) { COM_RELEASE(pLeft); STEREO_ERR("[DXGI] CreateTexture2D(right) failed: 0x%x", (unsigned)hr); return false; }

    sc->d3d11_left_tex  = pLeft;
    sc->d3d11_right_tex = pRight;
    STEREO_LOG("[DXGI] Eye textures created: L=%p R=%p  fmt=%u", pLeft, pRight, dxgi_fmt);
    return true;
}

/* ── Present one frame: copy Vulkan staging data → D3D11 → DXGI ──────── */
/*
 * stage_left / stage_right: CPU-visible pointers to the staged pixels for
 * left eye (layer 0) and right eye (layer 1) of this frame.
 * Each is app_width × app_height × 4 bytes (BGRA or RGBA, matching sc->format).
 */
VkResult dxgi_present_frame(StereoDevice *sd, StereoSwapchain *sc,
                             const void *stage_left, const void *stage_right)
{
    void *pCtx = sd->d3d11_ctx;
    UINT row_pitch = sc->app_width * 4;

    /* ── Upload left eye ──────────────────────────────────────────────── */
    /* ID3D11DeviceContext::UpdateSubresource = vtable[48] */
    ((void(WINAPI*)(void*, void*, UINT, void*, const void*, UINT, UINT))(*(void***)pCtx)[48])
    (pCtx, sc->d3d11_left_tex, 0, NULL, stage_left, row_pitch, 0);

    /* ── Upload right eye ─────────────────────────────────────────────── */
    ((void(WINAPI*)(void*, void*, UINT, void*, const void*, UINT, UINT))(*(void***)pCtx)[48])
    (pCtx, sc->d3d11_right_tex, 0, NULL, stage_right, row_pitch, 0);

    /* ── GetBuffer → DXGI back buffer Texture2DArray[2] ──────────────── */
    void *pBB = NULL;
    /* IDXGISwapChain::GetBuffer = vtable[9] */
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, const GUID*, void**))(*(void***)sc->dxgi_sc)[9])
                 (sc->dxgi_sc, 0, &IID_ID3D11Tex2D, &pBB);
    if (FAILED(hr) || !pBB) {
        STEREO_ERR("[DXGI] GetBuffer failed: 0x%x", (unsigned)hr);
        return VK_ERROR_DEVICE_LOST;
    }

    /* ── CopySubresourceRegion: left_tex → back-buffer slice 0 ─────── */
    /* vtable[46] = CopySubresourceRegion(dst, DstSub, DstX, DstY, DstZ, src, SrcSub, pBox) */
    ((void(WINAPI*)(void*, void*, UINT, UINT, UINT, UINT, void*, UINT, void*))(*(void***)pCtx)[46])
    (pCtx, pBB, 0, 0, 0, 0, sc->d3d11_left_tex,  0, NULL);

    /* ── CopySubresourceRegion: right_tex → back-buffer slice 1 ─────── */
    ((void(WINAPI*)(void*, void*, UINT, UINT, UINT, UINT, void*, UINT, void*))(*(void***)pCtx)[46])
    (pCtx, pBB, 1, 0, 0, 0, sc->d3d11_right_tex, 0, NULL);

    /* Release back buffer reference */
    COM_RELEASE(pBB);

    /* ── Present ─────────────────────────────────────────────────────── */
    /* IDXGISwapChain::Present = vtable[8] */
    hr = ((HRESULT(WINAPI*)(void*, UINT, UINT))(*(void***)sc->dxgi_sc)[8])
         (sc->dxgi_sc, 1, 0);

    if (hr == 0x087A0001 /* DXGI_STATUS_OCCLUDED */) {
        STEREO_LOG("[DXGI] Present: OCCLUDED (0x%x) — window not focused", (unsigned)hr);
        return VK_SUCCESS;  /* Non-fatal; app will keep rendering */
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

/* ── Cleanup swap-chain DXGI resources ───────────────────────────────── */
void dxgi_sc_destroy(StereoSwapchain *sc)
{
    COM_RELEASE(sc->d3d11_left_tex);
    COM_RELEASE(sc->d3d11_right_tex);
    COM_RELEASE(sc->dxgi_sc);
}

/* ── Cleanup device-level D3D11 + NVAPI ──────────────────────────────── */
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
