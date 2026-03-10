/*
 * present_alt.c — Alternative stereo presentation modes + hotkeys
 *
 * Modes implemented here:
 *   STEREO_PRESENT_DX9        — NvAPI + IDirect3D9Ex exclusive-fullscreen
 *   STEREO_PRESENT_SBS        — Side-by-side (D3D11 compose)
 *   STEREO_PRESENT_TAB        — Top-and-bottom (D3D11 compose)
 *   STEREO_PRESENT_INTERLACED — Row-interleaved (D3D11 compose)
 *
 * CPU staging flow:
 *   VkCmdCopyImageToBuffer (layer0 + layer1) → VkBuffer → cpu_map
 *   → D3D9 UpdateSurface / D3D11 UpdateSubresource
 *
 * D3D9 NvAPI IDs sourced from the NVAPI SDK header (stable ABI).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "present_alt.h"
#include "ini.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Minimal D3D9 interface definitions (no d3d9.h required)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define D3DFMT_X8R8G8B8      22
#define D3DFMT_A8R8G8B8      21
#define D3DSWAPEFFECT_DISCARD 1
#define D3DPOOL_DEFAULT       0
#define D3DPOOL_SYSTEMMEM     2
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x40
#define D3DCREATE_MULTITHREADED             0x4
#define D3DADAPTER_DEFAULT    0
#define D3DDEVTYPE_HAL        1
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000

typedef struct D3DPRESENT_PARAMETERS_ {
    UINT    BackBufferWidth, BackBufferHeight;
    UINT    BackBufferFormat;           /* D3DFORMAT */
    UINT    BackBufferCount;
    UINT    MultiSampleType;
    DWORD   MultiSampleQuality;
    UINT    SwapEffect;                 /* D3DSWAPEFFECT */
    HWND    hDeviceWindow;
    BOOL    Windowed;
    BOOL    EnableAutoDepthStencil;
    UINT    AutoDepthStencilFormat;
    DWORD   Flags;
    UINT    FullScreen_RefreshRateInHz;
    UINT    PresentationInterval;
} D3DPP;

typedef struct { int left, top, right, bottom; } D3DRECT_;
typedef struct { UINT Width, Height; } D3DSURFACE_DESC_;
typedef struct { void *pBits; INT Pitch; } D3DLOCKED_RECT_;

/* IDirect3D9Ex vtable offsets */
#define D3D9EX_CreateDeviceEx    20

/* IDirect3DDevice9 vtable offsets */
#define D3DDEV9_Release              2
#define D3DDEV9_Reset               16
#define D3DDEV9_Present             17
#define D3DDEV9_GetBackBuffer       18
#define D3DDEV9_StretchRect         34
#define D3DDEV9_CreateOffscreenPlainSurface 36

/* IDirect3DSurface9 vtable offsets */
#define D3DSURF_QueryInterface  0
#define D3DSURF_Release         2
#define D3DSURF_LockRect       13
#define D3DSURF_UnlockRect     14

#define D3DCALL(iface, idx, ...) \
    ((*(void***)(iface))[idx])

/* ═══════════════════════════════════════════════════════════════════════════
 * NVAPI additions for DX9 direct mode
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Function IDs (stable since Vulkan-era SDK) */
#define NvID_StereoSetDriverMode  0x5E8F1974u
#define NvID_StereoSetActiveEye   0x96EEA9F8u
#define NvID_StereoIsActivated    0x1FB0BC30u

/* NVAPI stereo driver mode */
#define NV_STEREO_MODE_AUTOMATIC  0
#define NV_STEREO_MODE_DIRECT     2

/* NVAPI eye selector */
#define NV_STEREO_EYE_RIGHT       1
#define NV_STEREO_EYE_LEFT        2

typedef void* (*PFN_NvQI_t)(unsigned);
static PFN_NvQI_t s_nvQI_alt = NULL;
static inline void* nv_q(unsigned id)
{ return s_nvQI_alt ? s_nvQI_alt(id) : NULL; }

typedef int (*PFN_NvInt)(int);
typedef int (*PFN_NvHandleEye)(void*, int);

/* dx9_nvapi_early_init is defined in stereo.c (DllMain translation unit)
 * so it is always linked, even in test builds that don't include present_alt.c.
 * dx9_init below uses s_nvQI_alt loaded from sd->nvapi_lib at device-create time. */


/* ═══════════════════════════════════════════════════════════════════════════
 * DXGI / D3D11 minimal definitions for compose mode
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef DXGI_FORMAT_B8G8R8A8_UNORM
#define DXGI_FORMAT_B8G8R8A8_UNORM 87
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#endif
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x20

static const GUID kIID_IDXGIDevice2 =
    {0x05008617,0xFBFD,0x4051,{0xA7,0x90,0x14,0x48,0x84,0xB4,0xF6,0xA9}};
static const GUID kIID_IDXGIFactory2 =
    {0x50C83A1C,0xE072,0x4C48,{0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0}};
static const GUID kIID_ID3D11Tex2D =
    {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};

#define DXGI2_CREATESWAP_HWND_IDX  15   /* IDXGIFactory2::CreateSwapChainForHwnd */

typedef struct DXGI_SWAP_CHAIN_DESC1_ {
    UINT   Width, Height;
    UINT   Format;       /* DXGI_FORMAT */
    BOOL   Stereo;
    struct { UINT Count, Quality; } SampleDesc;
    UINT   BufferUsage;
    UINT   BufferCount;
    UINT   Scaling;      /* DXGI_SCALING */
    UINT   SwapEffect;   /* DXGI_SWAP_EFFECT */
    UINT   AlphaMode;
    UINT   Flags;
} DXGI_SCD1_;

/* ID3D11DeviceContext::UpdateSubresource vtable[48] */
#define D3D11CTX_UpdateSubresource 48

/* ═══════════════════════════════════════════════════════════════════════════
 * Vulkan CPU staging helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t find_mem_type(StereoDevice *sd, uint32_t bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(sd->real_physdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

VkResult alt_alloc_stereo_image(StereoDevice *sd, StereoSwapchain *sc,
                                VkImage *out_image, VkDeviceMemory *out_mem)
{
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = sc->format,
        .extent        = { sc->app_width, sc->app_height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 2,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = sd->real.CreateImage(sd->real_device, &ici, NULL, out_image);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements mr;
    sd->real.GetImageMemoryRequirements(sd->real_device, *out_image, &mr);
    uint32_t mt = find_mem_type(sd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, out_mem);
    if (res != VK_SUCCESS) {
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return res;
    }
    res = sd->real.BindImageMemory(sd->real_device, *out_image, *out_mem, 0);
    if (res != VK_SUCCESS) {
        sd->real.FreeMemory(sd->real_device, *out_mem, NULL);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
    }
    return res;
}

VkResult alt_cpu_staging_init(StereoDevice *sd, StereoSwapchain *sc)
{
    if (sc->cpu_ok) return VK_SUCCESS;

    uint32_t eye_bytes = sc->app_width * sc->app_height * 4;
    sc->cpu_eye_bytes  = eye_bytes;
    VkDeviceSize total = (VkDeviceSize)eye_bytes * 2;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = total,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult res = sd->real.CreateBuffer(sd->real_device, &bci, NULL, &sc->cpu_buf);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements mr;
    sd->real.GetBufferMemoryRequirements(sd->real_device, sc->cpu_buf, &mr);
    uint32_t mt = find_mem_type(sd, mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) { sd->real.DestroyBuffer(sd->real_device, sc->cpu_buf, NULL); return VK_ERROR_OUT_OF_HOST_MEMORY; }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, &sc->cpu_mem);
    if (res != VK_SUCCESS) { sd->real.DestroyBuffer(sd->real_device, sc->cpu_buf, NULL); return res; }

    sd->real.BindBufferMemory(sd->real_device, sc->cpu_buf, sc->cpu_mem, 0);
    sd->real.MapMemory(sd->real_device, sc->cpu_mem, 0, total, 0, &sc->cpu_map);

    /* Command pool + buffer */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = sd->gfx_qf,
    };
    sd->real.CreateCommandPool(sd->real_device, &cpci, NULL, &sc->cpu_pool);
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->cpu_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    sd->real.AllocateCommandBuffers(sd->real_device, &cbai, &sc->cpu_cmd);

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    sd->real.CreateFence(sd->real_device, &fci, NULL, &sc->cpu_fence);

    sc->cpu_ok = true;
    STEREO_LOG("CPU staging init: %u bytes/eye (%ux%u)", eye_bytes, sc->app_width, sc->app_height);
    return VK_SUCCESS;
}

void alt_cpu_staging_destroy(StereoDevice *sd, StereoSwapchain *sc)
{
    if (!sc->cpu_ok) return;
    if (sc->cpu_fence) sd->real.DestroyFence(sd->real_device, sc->cpu_fence, NULL);
    if (sc->cpu_pool)  sd->real.DestroyCommandPool(sd->real_device, sc->cpu_pool, NULL);
    if (sc->cpu_map)   sd->real.UnmapMemory(sd->real_device, sc->cpu_mem);
    if (sc->cpu_buf)   sd->real.DestroyBuffer(sd->real_device, sc->cpu_buf, NULL);
    if (sc->cpu_mem)   sd->real.FreeMemory(sd->real_device, sc->cpu_mem, NULL);
    sc->cpu_ok = false;
}

VkResult alt_cpu_readback(StereoDevice *sd, StereoSwapchain *sc,
                          VkQueue queue,
                          uint32_t wait_sem_count,
                          const VkSemaphore *wait_sems,
                          VkImageLayout layout_in)
{
    if (!sc->cpu_ok) return VK_ERROR_INITIALIZATION_FAILED;

    sd->real.ResetFences(sd->real_device, 1, &sc->cpu_fence);
    sd->real.ResetCommandBuffer(sc->cpu_cmd, 0);

    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    sd->real.BeginCommandBuffer(sc->cpu_cmd, &begin);

    /* Transition both layers: layout_in → TRANSFER_SRC_OPTIMAL */
    VkImageMemoryBarrier b0 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = layout_in,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sc->stereo_images[0],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 },
    };
    sd->real.CmdPipelineBarrier(sc->cpu_cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &b0);

    /* Copy layer 0 (left) then layer 1 (right) to buffer */
    VkBufferImageCopy regions[2] = {
        { .bufferOffset = 0,
          .bufferRowLength = sc->app_width,
          .bufferImageHeight = sc->app_height,
          .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
          .imageOffset = {0,0,0},
          .imageExtent = { sc->app_width, sc->app_height, 1 } },
        { .bufferOffset = sc->cpu_eye_bytes,
          .bufferRowLength = sc->app_width,
          .bufferImageHeight = sc->app_height,
          .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1 },
          .imageOffset = {0,0,0},
          .imageExtent = { sc->app_width, sc->app_height, 1 } },
    };
    sd->real.CmdCopyImageToBuffer(sc->cpu_cmd, sc->stereo_images[0],
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->cpu_buf, 2, regions);

    /* Restore layout */
    VkImageMemoryBarrier b1 = b0;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b1.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b1.newLayout     = layout_in;
    sd->real.CmdPipelineBarrier(sc->cpu_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &b1);

    sd->real.EndCommandBuffer(sc->cpu_cmd);

    /* Build wait-stage masks for app semaphores */
    VkPipelineStageFlags *masks = NULL;
    if (wait_sem_count > 0) {
        masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
        if (!masks) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < wait_sem_count; i++)
            masks[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = wait_sem_count,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = masks,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &sc->cpu_cmd,
    };
    VkResult res = sd->real.QueueSubmit(queue, 1, &si, sc->cpu_fence);
    free(masks);
    if (res != VK_SUCCESS) return res;

    return sd->real.WaitForFences(sd->real_device, 1, &sc->cpu_fence, VK_TRUE, UINT64_MAX);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DX9 Direct Mode
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT sdkVersion, void **ppD3D);

bool dx9_init(StereoDevice *sd, StereoSwapchain *sc)
{
    if (sd->dx9_ok) return true;
    if (!sc->hwnd)  return false;

    /* Load d3d9.dll */
    HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) { STEREO_ERR("[DX9] d3d9.dll not found"); return false; }

    PFN_Direct3DCreate9Ex fnCreate =
        (PFN_Direct3DCreate9Ex)GetProcAddress(hD3D9, "Direct3DCreate9Ex");
    if (!fnCreate) {
        STEREO_ERR("[DX9] Direct3DCreate9Ex not found (Vista+ required)");
        FreeLibrary(hD3D9);
        return false;
    }

    /* NvAPI: s_nvQI_alt already initialised by dx9_nvapi_early_init() at
     * DLL_PROCESS_ATTACH.  SetDriverMode(DIRECT) was called there before any
     * D3D9 object was created, which is the only timing that works.
     * Just resolve it from sd->nvapi_lib as a fallback if early init didn't run. */
    if (!s_nvQI_alt && sd->nvapi_lib) {
        s_nvQI_alt = (PFN_NvQI_t)GetProcAddress((HMODULE)sd->nvapi_lib,
                                                   "nvapi_QueryInterface");
        STEREO_LOG("[DX9] nvapi_QueryInterface from sd->nvapi_lib (early init missed)");
    }

    /* Create IDirect3D9Ex */
    void *pD3D = NULL;
    HRESULT hr = fnCreate(32 /*D3D_SDK_VERSION*/, &pD3D);
    if (FAILED(hr) || !pD3D) {
        STEREO_ERR("[DX9] Direct3DCreate9Ex failed: 0x%x", (unsigned)hr);
        FreeLibrary(hD3D9);
        return false;
    }

    /* Build presentation parameters */
    uint32_t w = sc->app_width, h = sc->app_height;
    if (sd->stereo.override_width)  w = sd->stereo.override_width;
    if (sd->stereo.override_height) h = sd->stereo.override_height;
    uint32_t rr = sd->stereo.refresh_rate ? sd->stereo.refresh_rate : 120;

    D3DPP pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth             = w;
    pp.BackBufferHeight            = h;
    pp.BackBufferFormat            = D3DFMT_X8R8G8B8;
    pp.BackBufferCount             = 1;
    pp.SwapEffect                  = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow               = sc->hwnd;
    pp.Windowed                    = FALSE;
    pp.FullScreen_RefreshRateInHz  = rr;
    pp.PresentationInterval        = D3DPRESENT_INTERVAL_IMMEDIATE;

    void *pDev = NULL;
    /* IDirect3D9Ex::CreateDeviceEx = vtable[20] */
    typedef HRESULT (WINAPI *PFN_CDE)(void*, UINT, UINT, HWND, DWORD, D3DPP*, void*, void**);
    PFN_CDE fnCDE = (PFN_CDE)(*(void***)pD3D)[D3D9EX_CreateDeviceEx];
    hr = fnCDE(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, sc->hwnd,
               D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
               &pp, NULL, &pDev);
    if (FAILED(hr) || !pDev) {
        STEREO_ERR("[DX9] CreateDeviceEx failed: 0x%x — trying windowed fallback", (unsigned)hr);
        /* Try windowed fallback */
        pp.Windowed = TRUE;
        pp.FullScreen_RefreshRateInHz = 0;
        hr = fnCDE(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, sc->hwnd,
                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                   &pp, NULL, &pDev);
        if (FAILED(hr) || !pDev) {
            STEREO_ERR("[DX9] CreateDeviceEx windowed also failed: 0x%x", (unsigned)hr);
            ((void(WINAPI*)(void*))(*(void***)pD3D)[2])(pD3D);  /* Release */
            FreeLibrary(hD3D9);
            return false;
        }
    }
    STEREO_LOG("[DX9] Device created: %p  %ux%u @ %uHz  windowed=%d",
               pDev, w, h, rr, pp.Windowed);

    /* NvAPI stereo handle for the D3D9 device */
    void *hStereo = NULL;
    if (s_nvQI_alt) {
        typedef int (*PFN_NvFromIUnk)(void*, void**);
        PFN_NvFromIUnk fnCr = (PFN_NvFromIUnk)nv_q(0xAC7E37F4u /*NvID_StereoCreate*/);
        if (fnCr && fnCr(pDev, &hStereo) == 0) {
            typedef int (*PFN_NvH)(void*);
            PFN_NvH fnAct = (PFN_NvH)nv_q(0xF6A1AD68u /*NvID_StereoActivate*/);
            if (fnAct) { int r = fnAct(hStereo); STEREO_LOG("[DX9] NvAPI_Stereo_Activate = %d", r); }
        }
    }

    /* Create POOL_SYSTEMMEM staging surface (W×H, same format as back buffer) */
    void *pSurf = NULL;
    typedef HRESULT (WINAPI *PFN_COPS)(void*, UINT, UINT, UINT, UINT, void*, HANDLE);
    PFN_COPS fnCOPS = (PFN_COPS)(*(void***)pDev)[D3DDEV9_CreateOffscreenPlainSurface];
    hr = fnCOPS(pDev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &pSurf, NULL);
    if (FAILED(hr) || !pSurf) {
        STEREO_ERR("[DX9] CreateOffscreenPlainSurface failed: 0x%x", (unsigned)hr);
        /* Non-fatal — present path will try alternate method */
    }

    sd->dx9_lib      = hD3D9;
    sd->dx9_d3d      = pD3D;
    sd->dx9_dev      = pDev;
    sd->dx9_surf     = pSurf;
    sd->dx9_nvstereo = hStereo;
    sd->dx9_ok       = true;
    return true;
}

void dx9_destroy(StereoDevice *sd)
{
    if (!sd->dx9_ok) return;
    if (sd->dx9_nvstereo) {
        typedef int (*PFN_NvH)(void*);
        PFN_NvH fn = (PFN_NvH)nv_q(0x3A153134u /*NvID_StereoDestroy*/);
        if (fn) fn(sd->dx9_nvstereo);
    }
    if (sd->dx9_surf) ((void(WINAPI*)(void*))(*(void***)sd->dx9_surf)[D3DSURF_Release])(sd->dx9_surf);
    if (sd->dx9_dev)  ((void(WINAPI*)(void*))(*(void***)sd->dx9_dev)[D3DDEV9_Release])(sd->dx9_dev);
    if (sd->dx9_d3d)  ((void(WINAPI*)(void*))(*(void***)sd->dx9_d3d)[2])(sd->dx9_d3d);
    if (sd->dx9_lib)  FreeLibrary((HMODULE)sd->dx9_lib);
    sd->dx9_ok = false;
    sd->dx9_lib = sd->dx9_d3d = sd->dx9_dev = sd->dx9_surf = sd->dx9_nvstereo = NULL;
}

/* Upload cpu_map pixels to dx9_surf and present one eye. */
static HRESULT dx9_present_eye(StereoDevice *sd, StereoSwapchain *sc,
                               const void *pixels, int eye_id)
{
    void *pDev  = sd->dx9_dev;
    void *pSurf = sd->dx9_surf;
    if (!pDev) return E_FAIL;

    uint32_t w = sc->app_width, h = sc->app_height;

    /* Set active eye via NvAPI */
    if (sd->dx9_nvstereo && s_nvQI_alt) {
        PFN_NvHandleEye fnEye = (PFN_NvHandleEye)nv_q(NvID_StereoSetActiveEye);
        if (fnEye) fnEye(sd->dx9_nvstereo, eye_id);
    }

    /* Upload pixels to systemmem surface */
    if (pSurf && pixels) {
        typedef HRESULT (WINAPI *PFN_LR)(void*, D3DLOCKED_RECT_*, const RECT*, DWORD);
        typedef HRESULT (WINAPI *PFN_ULR)(void*);
        PFN_LR  fnLock   = (PFN_LR) (*(void***)pSurf)[D3DSURF_LockRect];
        PFN_ULR fnUnlock = (PFN_ULR)(*(void***)pSurf)[D3DSURF_UnlockRect];

        D3DLOCKED_RECT_ lr;
        if (SUCCEEDED(fnLock(pSurf, &lr, NULL, 0))) {
            const uint8_t *src = (const uint8_t *)pixels;
            uint8_t       *dst = (uint8_t *)lr.pBits;
            uint32_t row_bytes = w * 4;
            for (uint32_t y = 0; y < h; y++) {
                memcpy(dst + y * lr.Pitch, src + y * row_bytes, row_bytes);
            }
            fnUnlock(pSurf);
        }

        /* Get back buffer and UpdateSurface */
        void *pBB = NULL;
        typedef HRESULT (WINAPI *PFN_GBB)(void*, UINT, UINT, UINT, void**);
        PFN_GBB fnGBB = (PFN_GBB)(*(void***)pDev)[D3DDEV9_GetBackBuffer];
        if (SUCCEEDED(fnGBB(pDev, 0, 0, 0 /*D3DBACKBUFFER_TYPE_MONO*/, &pBB)) && pBB) {
            /* UpdateSurface: pSrc (SYSTEMMEM) → pDest (DEFAULT) */
            typedef HRESULT (WINAPI *PFN_US)(void*, void*, const RECT*, void*, const POINT*);
            /* UpdateSurface = vtable[30] on Device */
            PFN_US fnUS = (PFN_US)(*(void***)pDev)[30];
            fnUS(pDev, pSurf, NULL, pBB, NULL);
            ((void(WINAPI*)(void*))(*(void***)pBB)[D3DSURF_Release])(pBB);
        }
    }
    return S_OK;
}

VkResult dx9_present(StereoDevice *sd, StereoSwapchain *sc,
                     VkQueue queue,
                     uint32_t wait_sem_count,
                     const VkSemaphore *wait_sems)
{
    if (!sd->dx9_ok) return VK_ERROR_INITIALIZATION_FAILED;

    /* GPU readback both eye layers to CPU */
    VkResult res = alt_cpu_readback(sd, sc, queue, wait_sem_count, wait_sems,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (res != VK_SUCCESS) return res;

    const uint8_t *left  = (const uint8_t *)sc->cpu_map;
    const uint8_t *right = left + sc->cpu_eye_bytes;

    /* Present left then right eye, then flip */
    dx9_present_eye(sd, sc, left,  NV_STEREO_EYE_LEFT);
    dx9_present_eye(sd, sc, right, NV_STEREO_EYE_RIGHT);

    void *pDev = sd->dx9_dev;
    typedef HRESULT (WINAPI *PFN_PR)(void*, const RECT*, const RECT*, HWND, void*);
    PFN_PR fnPresent = (PFN_PR)(*(void***)pDev)[D3DDEV9_Present];
    HRESULT hr = fnPresent(pDev, NULL, NULL, NULL, NULL);
    if (FAILED(hr)) {
        STEREO_ERR("[DX9] Present failed: 0x%x", (unsigned)hr);
        /* Try device reset */
        return VK_SUCCESS; /* don't propagate as fatal to Vulkan */
    }

    /* Re-activate stereo each frame */
    if (sd->dx9_nvstereo && s_nvQI_alt) {
        typedef int (*PFN_NvH)(void*);
        PFN_NvH fnAct = (PFN_NvH)nv_q(0xF6A1AD68u);
        if (fnAct) fnAct(sd->dx9_nvstereo);
    }

    /* Half-FPS limiter for active shutter displays */
    if (sd->stereo.half_fps) Sleep(16);

    return VK_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Compose modes (SBS / TAB / Interlaced)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool compose_init(StereoDevice *sd, StereoSwapchain *sc)
{
    if (sd->comp_ok) return true;
    if (!sd->d3d11_ok || !sc->hwnd) return false;

    uint32_t w = sc->app_width, h = sc->app_height;

    /* Allocate CPU-side compose buffer (W×H×4) */
    sd->comp_composed = malloc((size_t)w * h * 4);
    if (!sd->comp_composed) return false;
    sd->comp_w = w;
    sd->comp_h = h;

    /* Get IDXGIFactory2 from the D3D11 device */
    void *pDXGIDevice = NULL;
    void *pAdapter    = NULL;
    void *pFactory    = NULL;
    HRESULT hr;

    typedef HRESULT (WINAPI *PFN_QI)(void*, const GUID*, void**);

    /* pDev->QueryInterface(IDXGIDevice) */
    PFN_QI fnQI = (PFN_QI)(*(void***)sd->d3d11_dev)[0];
    hr = fnQI(sd->d3d11_dev, &kIID_IDXGIDevice2, &pDXGIDevice);
    if (FAILED(hr) || !pDXGIDevice) goto fail;

    /* IDXGIDevice::GetAdapter = vtable[6] */
    typedef HRESULT (WINAPI *PFN_GA)(void*, void**);
    ((PFN_GA)(*(void***)pDXGIDevice)[6])(pDXGIDevice, &pAdapter);
    if (!pAdapter) goto fail_dev;

    /* IDXGIAdapter::GetParent(IDXGIFactory2) = vtable[5] */
    typedef HRESULT (WINAPI *PFN_GP)(void*, const GUID*, void**);
    ((PFN_GP)(*(void***)pAdapter)[5])(pAdapter, &kIID_IDXGIFactory2, &pFactory);
    if (!pFactory) goto fail_adap;

    /* CreateSwapChainForHwnd */
    DXGI_SCD1_ scd = {
        .Width        = w,
        .Height       = h,
        .Format       = DXGI_FORMAT_B8G8R8A8_UNORM,
        .Stereo       = FALSE,
        .SampleDesc   = {1, 0},
        .BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount  = 2,
        .Scaling      = 0, /* DXGI_SCALING_STRETCH */
        .SwapEffect   = 3, /* DXGI_SWAP_EFFECT_FLIP_DISCARD */
        .AlphaMode    = 0,
        .Flags        = 0,
    };
    typedef HRESULT (WINAPI *PFN_CSH)(void*, HWND, const DXGI_SCD1_*, void*, void*, void**);
    PFN_CSH fnCSH = (PFN_CSH)(*(void***)pFactory)[DXGI2_CREATESWAP_HWND_IDX];
    hr = fnCSH(pFactory, sc->hwnd, &scd, NULL, NULL, &sd->comp_sc);
    if (FAILED(hr) || !sd->comp_sc) {
        STEREO_ERR("[Compose] CreateSwapChainForHwnd failed: 0x%x", (unsigned)hr);
        goto fail_factory;
    }
    STEREO_LOG("[Compose] Swap chain created: %p  %ux%u", sd->comp_sc, w, h);
    sd->comp_ok = true;

fail_factory:
    ((void(WINAPI*)(void*))(*(void***)pFactory)[2])(pFactory);
fail_adap:
    ((void(WINAPI*)(void*))(*(void***)pAdapter)[2])(pAdapter);
fail_dev:
    ((void(WINAPI*)(void*))(*(void***)pDXGIDevice)[2])(pDXGIDevice);
fail:
    if (!sd->comp_ok) { free(sd->comp_composed); sd->comp_composed = NULL; }
    return sd->comp_ok;
}

void compose_destroy(StereoDevice *sd)
{
    if (sd->comp_sc) ((void(WINAPI*)(void*))(*(void***)sd->comp_sc)[2])(sd->comp_sc);
    free(sd->comp_composed);
    sd->comp_sc = NULL; sd->comp_composed = NULL; sd->comp_ok = false;
}

/* Write one composed frame (W×H BGRA) to the DXGI swap chain back buffer */
static VkResult compose_upload_and_present(StereoDevice *sd, uint32_t w, uint32_t h,
                                            const void *pixels)
{
    void *pBB = NULL;
    typedef HRESULT (WINAPI *PFN_GB)(void*, UINT, const GUID*, void**);
    HRESULT hr = ((PFN_GB)(*(void***)sd->comp_sc)[9])(
                 sd->comp_sc, 0, &kIID_ID3D11Tex2D, &pBB);
    if (FAILED(hr) || !pBB) return VK_ERROR_DEVICE_LOST;

    /* D3D11DeviceContext::UpdateSubresource(dest, 0, NULL, src, rowPitch, 0) */
    typedef void (WINAPI *PFN_US)(void*, void*, UINT, void*, const void*, UINT, UINT);
    PFN_US fnUS = (PFN_US)(*(void***)sd->d3d11_ctx)[D3D11CTX_UpdateSubresource];
    fnUS(sd->d3d11_ctx, pBB, 0, NULL, pixels, w * 4, 0);
    ((void(WINAPI*)(void*))(*(void***)pBB)[2])(pBB);

    /* IDXGISwapChain::Present */
    hr = ((HRESULT(WINAPI*)(void*, UINT, UINT))(*(void***)sd->comp_sc)[8])
         (sd->comp_sc, 0, 0);
    if (FAILED(hr)) { STEREO_ERR("[Compose] Present failed: 0x%x", (unsigned)hr); }
    return VK_SUCCESS;
}

VkResult compose_present(StereoDevice *sd, StereoSwapchain *sc,
                         VkQueue queue,
                         uint32_t wait_sem_count,
                         const VkSemaphore *wait_sems,
                         StereoPresentMode mode)
{
    if (!sd->comp_ok) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = alt_cpu_readback(sd, sc, queue, wait_sem_count, wait_sems,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (res != VK_SUCCESS) return res;

    uint32_t w = sc->app_width, h = sc->app_height;
    const uint8_t *left  = (const uint8_t *)sc->cpu_map;
    const uint8_t *right = left + sc->cpu_eye_bytes;
    uint8_t       *out   = (uint8_t *)sd->comp_composed;

    switch (mode) {

    case STEREO_PRESENT_SBS: {
        /* Left eye in left half-width, right eye in right half-width.
         * Each eye is horizontally scaled to w/2 via nearest-neighbour.    */
        uint32_t hw = w / 2;
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *lrow = left  + y * w * 4;
            const uint8_t *rrow = right + y * w * 4;
            uint8_t       *orow = out   + y * w * 4;
            for (uint32_t x = 0; x < hw; x++) {
                uint32_t sx = x * w / hw;
                memcpy(orow + x * 4,      lrow + sx * 4, 4);
                memcpy(orow + (hw + x)*4, rrow + sx * 4, 4);
            }
        }
        break;
    }

    case STEREO_PRESENT_TAB: {
        /* Left eye top half, right eye bottom half. */
        uint32_t hh = h / 2;
        for (uint32_t y = 0; y < hh; y++) {
            uint32_t sy = y * h / hh;
            memcpy(out + y * w * 4,        left  + sy * w * 4, w * 4);
            memcpy(out + (hh + y) * w * 4, right + sy * w * 4, w * 4);
        }
        break;
    }

    case STEREO_PRESENT_INTERLACED:
    default: {
        /* Even rows from left eye, odd rows from right eye. */
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src = (y & 1) ? right + y * w * 4 : left + y * w * 4;
            memcpy(out + y * w * 4, src, w * 4);
        }
        break;
    }
    }

    if (sd->stereo.half_fps) Sleep(16);
    return compose_upload_and_present(sd, w, h, out);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hotkeys
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Bit indices for our tracked key combos */
#define HOT_DEC_SEP   0
#define HOT_INC_SEP   1
#define HOT_DEC_CONV  2
#define HOT_INC_CONV  3
#define HOT_SAVE      4

static uint32_t sample_hotkeys(void)
{
    SHORT ctrl = GetAsyncKeyState(VK_CONTROL);
    if (!(ctrl & 0x8000)) return 0;  /* Ctrl not held */
    uint32_t mask = 0;
    if (GetAsyncKeyState(VK_F3) & 0x8000) mask |= (1u << HOT_DEC_SEP);
    if (GetAsyncKeyState(VK_F4) & 0x8000) mask |= (1u << HOT_INC_SEP);
    if (GetAsyncKeyState(VK_F5) & 0x8000) mask |= (1u << HOT_DEC_CONV);
    if (GetAsyncKeyState(VK_F6) & 0x8000) mask |= (1u << HOT_INC_CONV);
    if (GetAsyncKeyState(VK_F7) & 0x8000) mask |= (1u << HOT_SAVE);
    return mask;
}

void hotkeys_poll(StereoDevice *sd)
{
    uint32_t cur  = sample_hotkeys();
    uint32_t prev = sd->hotkey_prev;
    uint32_t rise = cur & ~prev;   /* keys newly pressed this frame */
    sd->hotkey_prev = cur;
    if (!rise) return;

    StereoConfig *cfg = &sd->stereo;
    bool changed = false;

    if (rise & (1u << HOT_DEC_SEP)) {
        cfg->separation -= cfg->step_separation;
        if (cfg->separation < 0.0f) cfg->separation = 0.0f;
        changed = true;
        STEREO_LOG("[Hotkey] Separation decreased → %.4f", cfg->separation);
    }
    if (rise & (1u << HOT_INC_SEP)) {
        cfg->separation += cfg->step_separation;
        changed = true;
        STEREO_LOG("[Hotkey] Separation increased → %.4f", cfg->separation);
    }
    if (rise & (1u << HOT_DEC_CONV)) {
        cfg->convergence -= cfg->step_convergence;
        changed = true;
        STEREO_LOG("[Hotkey] Convergence decreased → %.4f", cfg->convergence);
    }
    if (rise & (1u << HOT_INC_CONV)) {
        cfg->convergence += cfg->step_convergence;
        changed = true;
        STEREO_LOG("[Hotkey] Convergence increased → %.4f", cfg->convergence);
    }

    if (changed) {
        stereo_config_compute_offsets(cfg);
        /* Update the stereo UBO if it's mapped */
        if (sd->stereo_ubo_map) {
            float *p = (float *)sd->stereo_ubo_map;
            p[0] = cfg->left_eye_offset;
            p[1] = cfg->right_eye_offset;
            p[2] = cfg->convergence;
        }
    }

    if (rise & (1u << HOT_SAVE)) {
        /* Write separation + convergence to local INI */
        const char *ini = sd->local_ini[0] ? sd->local_ini : sd->global_ini;
        STEREO_LOG("[Hotkey] INI save: separation = %.6g -> %s",  cfg->separation,  ini);
        ini_write_float(ini, "VKS3D", "separation",  cfg->separation);
        STEREO_LOG("[Hotkey] INI save: convergence = %.6g -> %s", cfg->convergence, ini);
        ini_write_float(ini, "VKS3D", "convergence", cfg->convergence);
        STEREO_LOG("[Hotkey] INI save: complete");
        /* Brief visual feedback via a Windows beep */
        Beep(880, 80);
    }
}
