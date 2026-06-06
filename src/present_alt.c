/*
 * present_alt.c — Alternative stereo presentation modes + hotkeys
 *
 * Image-space stereo (SBS / TAB / Interlaced)
 * ────────────────────────────────────────────
 * gl_ViewIndex injection is non-functional on NVIDIA 426.06 for all shader
 * stages tested (VS, GS, synthesized TES).  Both rendered layers are
 * therefore identical.  Instead of relying on per-vertex binocular offsets,
 * compose_present applies a horizontal pixel shift to layer 0:
 *
 *   Left  eye (left  SBS half): sample from src[x + shift_px]
 *                                → content appears shifted left  (camera moved right)
 *   Right eye (right SBS half): sample from src[x - shift_px]
 *                                → content appears shifted right (camera moved left)
 *
 * shift_px = separation * scene_width / 2
 *
 * This creates genuine binocular disparity (both eyes see different images)
 * at uniform depth (screen-plane stereo, no per-depth variation).
 * The separation hotkeys (Ctrl+F3/F4) adjust shift_px in real time.
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
    UINT    BackBufferFormat;
    UINT    BackBufferCount;
    UINT    MultiSampleType;
    DWORD   MultiSampleQuality;
    UINT    SwapEffect;
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

#define D3D9EX_CreateDeviceEx    20
#define D3DDEV9_Release              2
#define D3DDEV9_Reset               16
#define D3DDEV9_Present             17
#define D3DDEV9_GetBackBuffer       18
#define D3DDEV9_StretchRect         34
#define D3DDEV9_CreateOffscreenPlainSurface 36
#define D3DSURF_QueryInterface  0
#define D3DSURF_Release         2
#define D3DSURF_LockRect       13
#define D3DSURF_UnlockRect     14

/* ═══════════════════════════════════════════════════════════════════════════
 * NVAPI additions for DX9 direct mode
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NvID_StereoSetDriverMode  0x5E8F1974u
#define NvID_StereoSetActiveEye   0x96EEA9F8u
#define NvID_StereoIsActivated    0x1FB0BC30u

#define NV_STEREO_MODE_AUTOMATIC  0
#define NV_STEREO_MODE_DIRECT     2
#define NV_STEREO_EYE_RIGHT       1
#define NV_STEREO_EYE_LEFT        2

typedef void* (*PFN_NvQI_t)(unsigned);
static PFN_NvQI_t s_nvQI_alt = NULL;
static inline void* nv_q(unsigned id)
{ return s_nvQI_alt ? s_nvQI_alt(id) : NULL; }

typedef int (*PFN_NvInt)(int);
typedef int (*PFN_NvHandleEye)(void*, int);

/* ═══════════════════════════════════════════════════════════════════════════
 * DXGI / D3D11 minimal definitions for compose mode
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef DXGI_FORMAT_B8G8R8A8_UNORM
#define DXGI_FORMAT_B8G8R8A8_UNORM 87
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#endif
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x20

static const GUID kIID_IDXGIDevice =
    {0x54EC77FA,0x1377,0x44E6,{0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C}};
static const GUID kIID_IDXGIFactory2 =
    {0x50C83A1C,0xE072,0x4C48,{0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0}};
static const GUID kIID_ID3D11Tex2D =
    {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};

#define DXGI2_CREATESWAP_HWND_IDX  15
#define D3D11CTX_UpdateSubresource 48

typedef struct DXGI_SWAP_CHAIN_DESC1_ {
    UINT   Width, Height;
    UINT   Format;
    BOOL   Stereo;
    struct { UINT Count, Quality; } SampleDesc;
    UINT   BufferUsage;
    UINT   BufferCount;
    UINT   Scaling;
    UINT   SwapEffect;
    UINT   AlphaMode;
    UINT   Flags;
} DXGI_SCD1_;

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
    if (mt == UINT32_MAX) {
        sd->real.DestroyBuffer(sd->real_device, sc->cpu_buf, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, &sc->cpu_mem);
    if (res != VK_SUCCESS) {
        sd->real.DestroyBuffer(sd->real_device, sc->cpu_buf, NULL);
        return res;
    }

    sd->real.BindBufferMemory(sd->real_device, sc->cpu_buf, sc->cpu_mem, 0);
    sd->real.MapMemory(sd->real_device, sc->cpu_mem, 0, total, 0, &sc->cpu_map);

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

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    sd->real.CreateFence(sd->real_device, &fci, NULL, &sc->cpu_fence);

    sc->cpu_ok = true;
    STEREO_LOG("CPU staging init: %u bytes/eye (%ux%u)", eye_bytes,
               sc->app_width, sc->app_height);
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

    /* gl_ViewIndex is broken on 426.06 — both layers are identical.
     * Read only layer 0; compose_present applies image-space stereo shift. */
    uint32_t layer_count = (sd->stereo.multiview && sd->multiview_pass_exists) ? 2 : 1;

    sd->real.ResetFences(sd->real_device, 1, &sc->cpu_fence);
    sd->real.ResetCommandBuffer(sc->cpu_cmd, 0);

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    sd->real.BeginCommandBuffer(sc->cpu_cmd, &begin);

    VkImageMemoryBarrier b0 = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = layout_in,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = sc->stereo_images[0],
        .subresourceRange    = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count
        },
    };
    sd->real.CmdPipelineBarrier(sc->cpu_cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &b0);

    VkBufferImageCopy regions[2] = {
        {
            .bufferOffset      = 0,
            .bufferRowLength   = sc->app_width,
            .bufferImageHeight = sc->app_height,
            .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset       = {0, 0, 0},
            .imageExtent       = { sc->app_width, sc->app_height, 1 },
        },
        {
            .bufferOffset      = sc->cpu_eye_bytes,
            .bufferRowLength   = sc->app_width,
            .bufferImageHeight = sc->app_height,
            .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1 },
            .imageOffset       = {0, 0, 0},
            .imageExtent       = { sc->app_width, sc->app_height, 1 },
        },
    };
    sd->real.CmdCopyImageToBuffer(sc->cpu_cmd, sc->stereo_images[0],
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->cpu_buf,
        layer_count, regions);

    VkImageMemoryBarrier b1 = b0;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b1.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b1.newLayout     = layout_in;
    b1.subresourceRange.layerCount = layer_count;
    sd->real.CmdPipelineBarrier(sc->cpu_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &b1);

    sd->real.EndCommandBuffer(sc->cpu_cmd);

    VkPipelineStageFlags *masks = NULL;
    if (wait_sem_count > 0) {
        masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
        if (!masks) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < wait_sem_count; i++)
            masks[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    VkSubmitInfo si = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = wait_sem_count,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = masks,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &sc->cpu_cmd,
    };
    VkResult res = sd->real.QueueSubmit(queue, 1, &si, sc->cpu_fence);
    free(masks);
    if (res != VK_SUCCESS) return res;

    return sd->real.WaitForFences(
        sd->real_device, 1, &sc->cpu_fence, VK_TRUE, UINT64_MAX);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DX9 Direct Mode
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT sdkVersion, void **ppD3D);

bool dx9_init(StereoDevice *sd, StereoSwapchain *sc)
{
    if (sd->dx9_ok) return true;
    if (!sc->hwnd)  return false;

    HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) { STEREO_ERR("[DX9] d3d9.dll not found"); return false; }

    PFN_Direct3DCreate9Ex fnCreate =
        (PFN_Direct3DCreate9Ex)GetProcAddress(hD3D9, "Direct3DCreate9Ex");
    if (!fnCreate) {
        STEREO_ERR("[DX9] Direct3DCreate9Ex not found");
        FreeLibrary(hD3D9);
        return false;
    }

    if (!s_nvQI_alt && sd->nvapi_lib) {
        s_nvQI_alt = (PFN_NvQI_t)GetProcAddress((HMODULE)sd->nvapi_lib,
                                                   "nvapi_QueryInterface");
    }

    void *pD3D = NULL;
    HRESULT hr = fnCreate(32, &pD3D);
    if (FAILED(hr) || !pD3D) {
        STEREO_ERR("[DX9] Direct3DCreate9Ex failed: 0x%x", (unsigned)hr);
        FreeLibrary(hD3D9);
        return false;
    }

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
    typedef HRESULT (WINAPI *PFN_CDE)(void*, UINT, UINT, HWND, DWORD, D3DPP*, void*, void**);
    PFN_CDE fnCDE = (PFN_CDE)(*(void***)pD3D)[D3D9EX_CreateDeviceEx];
    hr = fnCDE(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, sc->hwnd,
               D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
               &pp, NULL, &pDev);
    if (FAILED(hr) || !pDev) {
        STEREO_ERR("[DX9] CreateDeviceEx FSE failed: 0x%x, trying windowed", (unsigned)hr);
        pp.Windowed = TRUE;
        pp.FullScreen_RefreshRateInHz = 0;
        hr = fnCDE(pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, sc->hwnd,
                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                   &pp, NULL, &pDev);
        if (FAILED(hr) || !pDev) {
            STEREO_ERR("[DX9] CreateDeviceEx windowed failed: 0x%x", (unsigned)hr);
            ((void(WINAPI*)(void*))(*(void***)pD3D)[2])(pD3D);
            FreeLibrary(hD3D9);
            return false;
        }
    }
    STEREO_LOG("[DX9] Device: %p  %ux%u @ %uHz  windowed=%d", pDev, w, h, rr, pp.Windowed);

    void *hStereo = NULL;
    if (s_nvQI_alt) {
        typedef int (*PFN_NvFromIUnk)(void*, void**);
        PFN_NvFromIUnk fnCr = (PFN_NvFromIUnk)nv_q(0xAC7E37F4u);
        if (fnCr && fnCr(pDev, &hStereo) == 0) {
            typedef int (*PFN_NvH)(void*);
            PFN_NvH fnAct = (PFN_NvH)nv_q(0xF6A1AD68u);
            if (fnAct) { int r = fnAct(hStereo); STEREO_LOG("[DX9] NvAPI_Stereo_Activate=%d",r); }
        }
    }

    sd->dx9_lib      = hD3D9;
    sd->dx9_d3d      = pD3D;
    sd->dx9_dev      = pDev;
    sd->dx9_surf     = NULL;
    sd->dx9_nvstereo = hStereo;
    sd->dx9_ok       = true;
    return true;
}

void dx9_destroy(StereoDevice *sd)
{
    if (!sd->dx9_ok) return;
    if (sd->dx9_nvstereo && s_nvQI_alt) {
        typedef int (*PFN_NvH)(void*);
        PFN_NvH fn = (PFN_NvH)nv_q(0x3A153134u);
        if (fn) fn(sd->dx9_nvstereo);
    }
    if (sd->dx9_surf) ((void(WINAPI*)(void*))(*(void***)sd->dx9_surf)[D3DSURF_Release])(sd->dx9_surf);
    if (sd->dx9_dev)  ((void(WINAPI*)(void*))(*(void***)sd->dx9_dev)[D3DDEV9_Release])(sd->dx9_dev);
    if (sd->dx9_d3d)  ((void(WINAPI*)(void*))(*(void***)sd->dx9_d3d)[2])(sd->dx9_d3d);
    if (sd->dx9_lib)  FreeLibrary((HMODULE)sd->dx9_lib);
    sd->dx9_ok = false;
    sd->dx9_lib = sd->dx9_d3d = sd->dx9_dev = sd->dx9_surf = sd->dx9_nvstereo = NULL;
}

#define NVSTEREO_IMAGE_SIGNATURE 0x4433564eu
#define SIH_SWAP_EYES            0x00000001u
#define SIH_SCALE_TO_FIT         0x00000002u

typedef struct {
    unsigned int dwSignature;
    unsigned int dwWidth;
    unsigned int dwHeight;
    unsigned int dwBPP;
    unsigned int dwFlags;
} NVSTEREOIMAGEHEADER_;

static bool dx9_create_stereo_surface(StereoDevice *sd, uint32_t w, uint32_t h)
{
    typedef HRESULT (WINAPI *PFN_CRT)(void*, UINT, UINT, UINT,
                                      UINT, UINT, BOOL, void**, HANDLE*);
    PFN_CRT fnCRT = (PFN_CRT)(*(void***)sd->dx9_dev)[28];
    void *pSurf = NULL;
    HRESULT hr = fnCRT(sd->dx9_dev, w * 2, h + 1, D3DFMT_A8R8G8B8,
                       0, 0, TRUE, &pSurf, NULL);
    if (FAILED(hr) || !pSurf) {
        STEREO_ERR("[DX9] CreateRenderTarget(NV stereo,%ux%u) failed: 0x%x",
                   w*2,h+1,(unsigned)hr);
        return false;
    }

    D3DLOCKED_RECT_ lr;
    typedef HRESULT (WINAPI *PFN_LR)(void*, D3DLOCKED_RECT_*, const RECT*, DWORD);
    typedef HRESULT (WINAPI *PFN_ULR)(void*);
    PFN_LR  fnLock   = (PFN_LR) (*(void***)pSurf)[D3DSURF_LockRect];
    PFN_ULR fnUnlock = (PFN_ULR)(*(void***)pSurf)[D3DSURF_UnlockRect];

    if (SUCCEEDED(fnLock(pSurf, &lr, NULL, 0))) {
        uint8_t *data = (uint8_t *)lr.pBits;
        memset(data, 0, (size_t)lr.Pitch * h);
        NVSTEREOIMAGEHEADER_ *hdr =
            (NVSTEREOIMAGEHEADER_ *)(data + (size_t)lr.Pitch * h);
        hdr->dwSignature = NVSTEREO_IMAGE_SIGNATURE;
        hdr->dwWidth     = w * 2;
        hdr->dwHeight    = h;
        hdr->dwBPP       = 32;
        hdr->dwFlags     = 0;
        fnUnlock(pSurf);
    }

    sd->dx9_surf = pSurf;
    STEREO_LOG("[DX9] NV stereo surface: %ux%u", w*2, h+1);
    return true;
}

VkResult dx9_present(StereoDevice *sd, StereoSwapchain *sc,
                     VkQueue queue,
                     uint32_t wait_sem_count,
                     const VkSemaphore *wait_sems)
{
    if (!sd->dx9_ok) return VK_ERROR_INITIALIZATION_FAILED;

    if (!sd->dx9_surf) {
        if (!dx9_create_stereo_surface(sd, sc->app_width, sc->app_height))
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = alt_cpu_readback(sd, sc, queue, wait_sem_count, wait_sems,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (res != VK_SUCCESS) return res;

    const uint8_t *left  = (const uint8_t *)sc->cpu_map;
    const uint8_t *right = (sd->stereo.multiview && sd->multiview_pass_exists)
                           ? left + sc->cpu_eye_bytes : left;

    uint32_t w = sc->app_width, h = sc->app_height;
    void *pSurf = sd->dx9_surf;
    {
        typedef HRESULT (WINAPI *PFN_LR)(void*, D3DLOCKED_RECT_*, const RECT*, DWORD);
        typedef HRESULT (WINAPI *PFN_ULR)(void*);
        PFN_LR  fnLock   = (PFN_LR) (*(void***)pSurf)[D3DSURF_LockRect];
        PFN_ULR fnUnlock = (PFN_ULR)(*(void***)pSurf)[D3DSURF_UnlockRect];

        D3DLOCKED_RECT_ locked;
        if (SUCCEEDED(fnLock(pSurf, &locked, NULL, 0))) {
            uint8_t *dst = (uint8_t *)locked.pBits;
            size_t row_bytes = (size_t)w * 4;
            size_t stride_out = (size_t)locked.Pitch;
            for (uint32_t y = 0; y < h; y++) {
                uint8_t *row = dst + y * stride_out;
                memcpy(row,             right + y * row_bytes, row_bytes);
                memcpy(row + row_bytes, left  + y * row_bytes, row_bytes);
            }
            fnUnlock(pSurf);
        }
    }

    void *pDev = sd->dx9_dev;
    void *pBB  = NULL;
    typedef HRESULT (WINAPI *PFN_GBB)(void*, UINT, UINT, UINT, void**);
    ((PFN_GBB)(*(void***)pDev)[D3DDEV9_GetBackBuffer])(pDev, 0, 0, 0, &pBB);
    if (pBB) {
        typedef HRESULT (WINAPI *PFN_SR)(void*, void*, const RECT*, void*, const RECT*, UINT);
        ((PFN_SR)(*(void***)pDev)[D3DDEV9_StretchRect])(pDev, pSurf, NULL, pBB, NULL, 2);
        ((void(WINAPI*)(void*))(*(void***)pBB)[D3DSURF_Release])(pBB);
    }

    typedef HRESULT (WINAPI *PFN_PR)(void*, const RECT*, const RECT*, HWND, void*);
    HRESULT hr = ((PFN_PR)(*(void***)pDev)[D3DDEV9_Present])(pDev, NULL, NULL, NULL, NULL);
    if (FAILED(hr)) STEREO_ERR("[DX9] Present failed: 0x%x", (unsigned)hr);

    if (sd->stereo.half_fps) Sleep(16);
    return VK_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Compose modes (SBS / TAB / Interlaced)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool compose_init(StereoDevice *sd, StereoSwapchain *sc)
{
    if (sd->comp_ok) return true;
    if (!sc->hwnd) return false;

    uint32_t w = sc->app_width, h = sc->app_height;
    sd->comp_composed = malloc((size_t)w * h * 4);
    if (!sd->comp_composed) return false;

    sd->comp_hwnd = sc->hwnd;
    sd->comp_w    = w;
    sd->comp_h    = h;
    sd->comp_ok   = true;
    STEREO_LOG("[Compose] init: GDI StretchDIBits  hwnd=%p  %ux%u", sc->hwnd, w, h);
    return true;
}

void compose_destroy(StereoDevice *sd)
{
    free(sd->comp_composed);
    sd->comp_composed = NULL;
    sd->comp_hwnd = NULL;
    sd->comp_ok = false;
}

static VkResult compose_upload_and_present(StereoDevice *sd, uint32_t w, uint32_t h,
                                            const void *pixels)
{
    HWND hwnd = sd->comp_hwnd;
    if (!hwnd) return VK_ERROR_INITIALIZATION_FAILED;

    HDC hdc = GetDC(hwnd);
    if (!hdc) return VK_ERROR_DEVICE_LOST;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)w;
    bmi.bmiHeader.biHeight      = -(LONG)h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = 0;

    StretchDIBits(hdc, 0, 0, (int)w, (int)h,
                  0, 0, (int)w, (int)h,
                  pixels, &bmi, 0, SRCCOPY);

    ReleaseDC(hwnd, hdc);
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

    /* When gl_ViewIndex worked correctly, both layers contain unique per-view
     * renders: layer 0 = left eye, layer 1 = right eye.  Use them directly.
     * Fall back to image-space shift of layer 0 when only one layer is valid
     * (e.g. multiview disabled, or driver did not populate gl_ViewIndex).   */
    bool two_layers = sd->stereo.multiview && sd->multiview_pass_exists;
    const uint8_t *left_eye  = (const uint8_t *)sc->cpu_map;
    const uint8_t *right_eye = two_layers ? left_eye + sc->cpu_eye_bytes : left_eye;
    uint8_t *out = (uint8_t *)sd->comp_composed;

    switch (mode) {

    case STEREO_PRESENT_SBS: {
        uint32_t hw = w / 2;
        /* two_layers: no shift needed — each layer is a distinct eye render.
         * single layer: apply image-space shift to fake depth from screen plane. */
        int shift_px = two_layers ? 0 : (int)(sd->stereo.separation * (float)w * 0.5f);
        STEREO_LOG("[Compose SBS] two_layers=%d  shift_px=%d  sep=%.4f  w=%u",
                   (int)two_layers, shift_px, sd->stereo.separation, w);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *lrow = left_eye  + y * w * 4;
            const uint8_t *rrow = right_eye + y * w * 4;
            uint8_t       *orow = out + y * w * 4;
            for (uint32_t x = 0; x < hw; x++) {
                int sx = (int)((uint32_t)x * w / hw);
                int sl = sx + shift_px; if (sl >= (int)w) sl = (int)w - 1;
                int sr = sx - shift_px; if (sr < 0) sr = 0;
                memcpy(orow + x * 4,        lrow + sl * 4, 4);
                memcpy(orow + (hw + x) * 4, rrow + sr * 4, 4);
            }
        }
        break;
    }

    case STEREO_PRESENT_TAB: {
        uint32_t hh = h / 2;
        int shift_py = two_layers ? 0 : (int)(sd->stereo.separation * (float)h * 0.5f);
        for (uint32_t y = 0; y < hh; y++) {
            int sy = (int)((uint32_t)y * h / hh);
            int sl = sy + shift_py; if (sl >= (int)h) sl = (int)h - 1;
            int sr = sy - shift_py; if (sr < 0) sr = 0;
            memcpy(out + y * w * 4,        left_eye  + sl * w * 4, w * 4);
            memcpy(out + (hh + y) * w * 4, right_eye + sr * w * 4, w * 4);
        }
        break;
    }

    case STEREO_PRESENT_INTERLACED:
    default: {
        int shift_px = two_layers ? 0 : (int)(sd->stereo.separation * (float)w * 0.5f);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *erow = ((y & 1) ? right_eye : left_eye) + y * w * 4;
            uint8_t       *orow = out + y * w * 4;
            int shift = two_layers ? 0 : ((y & 1) ? -shift_px : shift_px);
            for (uint32_t x = 0; x < w; x++) {
                int sx = (int)x + shift;
                if (sx < 0) sx = 0; if (sx >= (int)w) sx = (int)w - 1;
                memcpy(orow + x * 4, erow + sx * 4, 4);
            }
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

#define HOT_DEC_SEP   0
#define HOT_INC_SEP   1
#define HOT_DEC_CONV  2
#define HOT_INC_CONV  3
#define HOT_SAVE      4

static uint32_t sample_hotkeys(void)
{
    SHORT ctrl = GetAsyncKeyState(VK_CONTROL);
    if (!(ctrl & 0x8000)) return 0;
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
    uint32_t rise = cur & ~prev;
    sd->hotkey_prev = cur;
    if (!rise) return;

    StereoConfig *cfg = &sd->stereo;
    bool changed = false;

    if (rise & (1u << HOT_DEC_SEP)) {
        cfg->separation -= cfg->step_separation;
        if (cfg->separation < 0.0f) cfg->separation = 0.0f;
        changed = true;
        STEREO_LOG("[Hotkey] Separation → %.4f", cfg->separation);
    }
    if (rise & (1u << HOT_INC_SEP)) {
        cfg->separation += cfg->step_separation;
        changed = true;
        STEREO_LOG("[Hotkey] Separation → %.4f", cfg->separation);
    }
    if (rise & (1u << HOT_DEC_CONV)) {
        cfg->convergence -= cfg->step_convergence;
        changed = true;
        STEREO_LOG("[Hotkey] Convergence → %.4f", cfg->convergence);
    }
    if (rise & (1u << HOT_INC_CONV)) {
        cfg->convergence += cfg->step_convergence;
        changed = true;
        STEREO_LOG("[Hotkey] Convergence → %.4f", cfg->convergence);
    }

    if (changed) {
        stereo_config_compute_offsets(cfg);
        if (sd->stereo_ubo_map) {
            float *p = (float *)sd->stereo_ubo_map;
            p[0] = cfg->left_eye_offset;
            p[1] = cfg->right_eye_offset;
            p[2] = cfg->convergence;
        }
    }

    if (rise & (1u << HOT_SAVE)) {
        const char *ini = sd->local_ini[0] ? sd->local_ini : sd->global_ini;
        ini_write_float(ini, "VKS3D", "separation",  cfg->separation);
        ini_write_float(ini, "VKS3D", "convergence", cfg->convergence);
        STEREO_LOG("[Hotkey] Saved sep=%.4f conv=%.4f → %s",
                   cfg->separation, cfg->convergence, ini);
        Beep(880, 80);
    }
}
