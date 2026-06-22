/*
 * swapchain.c — GPU blit SBS stereo swapchain
 *
 * External-memory + multiview architecture (DXGI path) plus
 * GPU-blit compose path (SBS/TAB/Interlaced) replacing the old CPU readback.
 *
 * Compose path frame flow:
 *   App renders → stereo_images[0] (layers 0=left, 1=right)
 *   QueuePresentKHR → gpu_compose_present:
 *     AcquireNextImageKHR (real SC) → barrier → CmdBlitImage × 2 → Present
 *   AcquireNextImageKHR (fake) → WaitForFences(barrier_fences[0])
 *     ensures stereo_images[0] is safe for next render
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"
#include "present_alt.h"
#include "present_nv3d.h"

/* ── Helper: find suitable memory type ──────────────────────────────────── */
static uint32_t find_memory_type(StereoDevice *sd, uint32_t type_bits,
                                  VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(sd->real_physdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

/* ── Allocate VkImage backed by imported D3D11 NT-handle memory ─────────── */
static VkResult alloc_external_stereo_image(StereoDevice *sd, StereoSwapchain *sc,
                                             VkImage *out_image, VkDeviceMemory *out_mem)
{
    VkExternalMemoryImageCreateInfo ext_img = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
    };
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = &ext_img,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = sc->format,
        .extent        = {sc->app_width, sc->app_height, 1},
        .mipLevels     = 1,
        .arrayLayers   = 2,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = sd->real.CreateImage(sd->real_device, &ici, NULL, out_image);
    if (res != VK_SUCCESS) { STEREO_ERR("CreateImage(external) failed: %d", res); return res; }

    VkMemoryRequirements mr;
    sd->real.GetImageMemoryRequirements(sd->real_device, *out_image, &mr);
    if (!sd->real.GetMemoryWin32HandlePropertiesKHR) {
        STEREO_ERR("GetMemoryWin32HandlePropertiesKHR not loaded");
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkMemoryWin32HandlePropertiesKHR hp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    res = sd->real.GetMemoryWin32HandlePropertiesKHR(sd->real_device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, sc->shared_nt_handle, &hp);
    if (res != VK_SUCCESS) {
        STEREO_ERR("GetMemoryWin32HandlePropertiesKHR failed: %d", res);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL); return res;
    }
    uint32_t mt = find_memory_type(sd, mr.memoryTypeBits & hp.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        STEREO_ERR("No compatible memory for external image");
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkImportMemoryWin32HandleInfoKHR import_info = {
        .sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        .handle     = sc->shared_nt_handle,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import_info,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, out_mem);
    if (res != VK_SUCCESS) {
        STEREO_ERR("AllocateMemory(import) failed: %d", res);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL); return res;
    }
    sc->shared_nt_handle = NULL;
    res = sd->real.BindImageMemory(sd->real_device, *out_image, *out_mem, 0);
    if (res != VK_SUCCESS) {
        STEREO_ERR("BindImageMemory(external) failed: %d", res);
        sd->real.FreeMemory(sd->real_device, *out_mem, NULL);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
    }
    return res;
}

/* ── Barrier CB + fence for frame sync ──────────────────────────────────── */
static bool setup_barrier_resources(StereoDevice *sd, StereoSwapchain *sc)
{
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = sd->gfx_qf,
    };
    if (sd->real.CreateCommandPool(sd->real_device, &cpci, NULL, &sc->barrier_pool)
            != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->barrier_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (sd->real.AllocateCommandBuffers(sd->real_device, &cbai, sc->barrier_cmds)
            != VK_SUCCESS) return false;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,  /* starts signaled → first acquire never blocks */
    };
    return sd->real.CreateFence(sd->real_device, &fci, NULL, &sc->barrier_fences[0])
           == VK_SUCCESS;
}

/* ── Allocate stereo render target (2-layer image + view, no CPU staging) ─ */
static VkResult alloc_alt_stereo_swapchain(StereoDevice *sd, StereoSwapchain *sc)
{
    sc->image_count      = 1;
    sc->stereo_images    = calloc(1, sizeof(VkImage));
    sc->stereo_memory    = calloc(1, sizeof(VkDeviceMemory));
    sc->stereo_views_arr = calloc(1, sizeof(VkImageView));
    sc->barrier_cmds     = calloc(1, sizeof(VkCommandBuffer));
    sc->barrier_fences   = calloc(1, sizeof(VkFence));
    if (!sc->stereo_images || !sc->stereo_memory || !sc->stereo_views_arr ||
        !sc->barrier_cmds  || !sc->barrier_fences)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkResult res = alt_alloc_stereo_image(sd, sc,
                       &sc->stereo_images[0], &sc->stereo_memory[0]);
    if (res != VK_SUCCESS) return res;

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = sc->stereo_images[0],
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format   = sc->format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 },
    };
    res = sd->real.CreateImageView(sd->real_device, &vci, NULL, &sc->stereo_views_arr[0]);
    if (res != VK_SUCCESS) return res;

    STEREO_LOG(
        "[NV3D TEST] alloc_alt_stereo_swapchain image=%p view=%p count=%u",
        sc->stereo_images[0],
        sc->stereo_views_arr[0],
        sc->image_count);

    /* CPU staging NOT created here — caller adds it for DX9, not for GPU compose */
    return VK_SUCCESS;
}

/* ── image untracking helper ─ */
static void remove_tracked_image(
    VkImage *arr,
    uint32_t *count,
    VkImage image)
{
    STEREO_LOG(
        "[IMAGE TRACK SEARCH] image=%p count=%u",
        image,
        *count);
    for (uint32_t i = 0; i < *count; i++)
    {
        if (arr[i] == image)
        {
            uint32_t last = --(*count);
            arr[i] = arr[last];

            STEREO_LOG(
                "[IMAGE TRACK REMOVE] image=%p slot=%u count=%u",
                image,
                i,
                *count);

            return;
        }
    }
    STEREO_LOG(
        "[IMAGE TRACK MISS] image=%p count=%u",
        image,
        *count);
}


/* ── vkCreateSwapchainKHR ──────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateSwapchainKHR(VkDevice device,
                          const VkSwapchainCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks    *pAllocator,
                          VkSwapchainKHR                 *pSwapchain)
{

    STEREO_LOG(
        "[CREATE SC] surface=%p old=%p",
        pCreateInfo->surface,
        pCreateInfo->oldSwapchain);
    StereoDevice *sd = stereo_device_from_handle(device);
    STEREO_LOG(
        "[CREATE SC START] count=%u old=%p",
        sd->swapchain_count,
        pCreateInfo->oldSwapchain);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    if (!sd->stereo.enabled || sd->swapchain_count >= MAX_SWAPCHAINS)
        return sd->real.CreateSwapchainKHR(sd->real_device, pCreateInfo, pAllocator, pSwapchain);

    uint32_t app_w = pCreateInfo->imageExtent.width;
    uint32_t app_h = pCreateInfo->imageExtent.height;

    STEREO_LOG(
        "[CREATE SC] swapchain_count=%u old=%p",
        sd->swapchain_count,
        pCreateInfo->oldSwapchain);

    StereoSwapchain *old_sc = NULL;

    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
    {
        old_sc =
            stereo_swapchain_lookup(
                sd,
                pCreateInfo->oldSwapchain);

        STEREO_LOG(
            "[CREATE SC OLD LOOKUP] old=%p old_sc=%p",
            pCreateInfo->oldSwapchain,
            old_sc);
    }
    
    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
    {
        old_sc =
            stereo_swapchain_lookup(
                sd,
                pCreateInfo->oldSwapchain);
    
        STEREO_LOG(
            "[CREATE SC OLD LOOKUP] old=%p old_sc=%p",
            pCreateInfo->oldSwapchain,
            old_sc);
    }
    
    StereoSwapchain *sc;
    
    if (old_sc)
    {
        sc = old_sc;
        sc->resize_reused = true;
    
        STEREO_LOG(
            "[CREATE SC REUSE] sc=%p",
            sc);
    }
    else
    {
    sc = &sd->swapchains[sd->swapchain_count];

    memset(sc, 0, sizeof(*sc));
    sc->resize_reused = false;
    STEREO_LOG(
        "[CREATE SC NEW] sc=%p count=%u reused=%d",
        sc,
        sd->swapchain_count,
        (int)sc->resize_reused);
    }

    sc->device     = sd->real_device;
    sc->app_width  = app_w;
    sc->app_height = app_h;
    sc->format     = pCreateInfo->imageFormat;
    sc->hwnd       = stereo_si_hwnd_for_surface(sd->si, pCreateInfo->surface);

    STEREO_LOG(
        "CreateSwapchain: enabled=%d present_mode=%d",
        (int)sd->stereo.enabled,
        (int)sd->stereo.present_mode);

    StereoPresentMode req = sd->stereo.present_mode;

    STEREO_LOG(
        "CreateSwapchain: req=%d stereo.enabled=%d",
        (int)req,
        (int)sd->stereo.enabled);

    if (req == STEREO_PRESENT_NV3DLIB)
    {
        STEREO_LOG("[NV3D] requested");

        if (!nv3d_init(sd, app_w, app_h))
        {
            STEREO_ERR("[NV3D] init failed");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (sc->stereo_images)
        {
            STEREO_LOG(
                "[NV3D] stereo_image[0]=%p",
                sc->stereo_images[0]);
        }
        STEREO_LOG("[NV3D] init succeeded");

        VkResult nvres =
            alloc_alt_stereo_swapchain(sd, sc);

        if (nvres == VK_SUCCESS)
        {
            if (!setup_barrier_resources(sd, sc))
            {
                STEREO_ERR(
                    "[NV3D] setup_barrier_resources failed");

                nvres = VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        STEREO_LOG(
            "[NV3D] alloc_alt_stereo_swapchain=%d image_count=%u images=%p",
            nvres,
            sc->image_count,
            sc->stereo_images);

        STEREO_LOG(
            "[NV3D] after alloc cmds=%p fences=%p",
            sc->barrier_cmds,
            sc->barrier_fences);

        if (nvres != VK_SUCCESS)
        {
            STEREO_ERR(
                "[NV3D] alloc_alt_stereo_swapchain failed");
            goto passthrough;
        }
        sc->present_mode  = STEREO_PRESENT_NV3DLIB;
        sc->stereo_active = true;
        sc->real_swapchain = VK_NULL_HANDLE;
        *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
        sc->app_handle = *pSwapchain;
        STEREO_LOG(
            "[CREATE SC] sc=%p app_handle=%p returned=%p",
            sc,
            sc->app_handle,
            *pSwapchain);
        if (pCreateInfo->oldSwapchain == VK_NULL_HANDLE)
            sd->swapchain_count++;

        STEREO_LOG(
            "[NV3D] RETURNING NV3D SWAPCHAIN handle=%p",
            (void*)*pSwapchain);

        STEREO_LOG(
            "[NV3D] return cmds=%p fences=%p",
            sc->barrier_cmds,
            sc->barrier_fences);

        if (sc->barrier_cmds)
        {
            STEREO_LOG(
                "[NV3D] cmd0=%p",
                sc->barrier_cmds[0]);
        }

        if (sc->barrier_fences)
        {
            STEREO_LOG(
                "[NV3D] fence0=%p",
                sc->barrier_fences[0]);
        }
        return VK_SUCCESS;
    }

    STEREO_LOG(
        "CreateSwapchain: req=%d stereo.enabled=%d",
        (int)req,
        (int)sd->stereo.enabled);

    /* ── DXGI 1.2 + external memory ─────────────────────────────────── */
    if (req == STEREO_PRESENT_AUTO || req == STEREO_PRESENT_DXGI) {
        bool dxgi_ok = false;
        HANDLE nt_handle = NULL;
        if (sd->dxgi_init_in_progress) goto passthrough;
        if (sc->hwnd && dxgi_device_init(sd)) {
            dxgi_stereo_activate(sd);
            sd->dxgi_init_in_progress = true;
            if (dxgi_sc_create(sd, sc, &nt_handle)) {
                dxgi_ok = true;
                sd->dxgi_init_in_progress = false;
            } else {
                sd->dxgi_init_in_progress = false;
                dxgi_sc_destroy(sc);
            }
        }
        if (dxgi_ok) {
            sc->image_count      = 1;
            sc->stereo_images    = calloc(1, sizeof(VkImage));
            sc->stereo_memory    = calloc(1, sizeof(VkDeviceMemory));
            sc->stereo_views_arr = calloc(1, sizeof(VkImageView));
            sc->barrier_cmds     = calloc(1, sizeof(VkCommandBuffer));
            sc->barrier_fences   = calloc(1, sizeof(VkFence));
            if (!sc->stereo_images || !sc->stereo_memory || !sc->stereo_views_arr ||
                !sc->barrier_cmds  || !sc->barrier_fences) {
                dxgi_sc_destroy(sc);
                if (req == STEREO_PRESENT_DXGI) goto passthrough;
                goto try_dx9;
            }
            VkResult res = alloc_external_stereo_image(sd, sc,
                &sc->stereo_images[0], &sc->stereo_memory[0]);
            if (res != VK_SUCCESS) {
                dxgi_sc_destroy(sc);
                if (req == STEREO_PRESENT_DXGI) goto passthrough;
                goto try_dx9;
            }
            VkImageViewCreateInfo vci = {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image    = sc->stereo_images[0],
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format   = sc->format,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 },
            };
            res = sd->real.CreateImageView(sd->real_device, &vci, NULL, &sc->stereo_views_arr[0]);
            if (res != VK_SUCCESS) { dxgi_sc_destroy(sc); goto try_dx9; }
            if (!setup_barrier_resources(sd, sc)) { dxgi_sc_destroy(sc); goto try_dx9; }
            sc->present_mode  = STEREO_PRESENT_DXGI;
            sc->dxgi_mode     = true;
            sc->stereo_active = true;
            sc->real_swapchain = VK_NULL_HANDLE;
            *pSwapchain       = (VkSwapchainKHR)(uintptr_t)sc;
            sc->app_handle    = *pSwapchain;
            STEREO_LOG(
                "[CREATE SC] sc=%p app_handle=%p returned=%p",
                sc,
                sc->app_handle,
                *pSwapchain);
            sd->stereo_w = app_w; sd->stereo_h = app_h;
            if (pCreateInfo->oldSwapchain == VK_NULL_HANDLE)
                sd->swapchain_count++;
            STEREO_LOG("DXGI stereo swapchain (external mem): %ux%u  handle=%p",
                       app_w, app_h, (void*)*pSwapchain);
            return VK_SUCCESS;
        }
        if (req == STEREO_PRESENT_DXGI) { STEREO_ERR("DXGI forced but failed"); goto passthrough; }
    }

try_dx9:
    if (req == STEREO_PRESENT_AUTO || req == STEREO_PRESENT_DX9) {
        if (!sd->d3d11_ok) dxgi_device_init(sd);
        if (sc->hwnd && dx9_init(sd, sc)) {
            VkResult res = alloc_alt_stereo_swapchain(sd, sc);
            if (res == VK_SUCCESS) res = alt_cpu_staging_init(sd, sc); /* DX9 needs CPU staging */
            if (res == VK_SUCCESS && setup_barrier_resources(sd, sc)) {
                sc->present_mode  = STEREO_PRESENT_DX9;
                sc->dxgi_mode     = false;
                sc->stereo_active = true;
                sc->real_swapchain = VK_NULL_HANDLE;
                *pSwapchain       = (VkSwapchainKHR)(uintptr_t)sc;
                sc->app_handle    = *pSwapchain;
                STEREO_LOG(
                    "[CREATE SC] sc=%p app_handle=%p returned=%p",
                    sc,
                    sc->app_handle,
                    *pSwapchain);
                sd->stereo_w = app_w; sd->stereo_h = app_h;
                if (pCreateInfo->oldSwapchain == VK_NULL_HANDLE)
                    sd->swapchain_count++;
                STEREO_LOG("DX9 stereo swapchain: %ux%u  handle=%p", app_w, app_h, (void*)*pSwapchain);
                return VK_SUCCESS;
            }
        }
        if (req == STEREO_PRESENT_DX9) { STEREO_ERR("DX9 forced but failed"); goto passthrough; }
        req = STEREO_PRESENT_SBS;
    }

    /* ── GPU blit compose (SBS / TAB / Interlaced) ───────────────────── */
    if (req == STEREO_PRESENT_SBS  ||
        req == STEREO_PRESENT_TAB  ||
        req == STEREO_PRESENT_INTERLACED) {
        STEREO_LOG("[SBS] gpu_compose_sc_init surface=%p", (void*)(uintptr_t)pCreateInfo->surface);
        STEREO_LOG(
            "[CREATE SC GPU] hwnd=%p surface=%p",
            sc->hwnd,
            pCreateInfo->surface);
        if (sc->hwnd && gpu_compose_sc_init(sd, sc, pCreateInfo->surface)) {
            VkResult res = alloc_alt_stereo_swapchain(sd, sc);
            /* No CPU staging — GPU blit reads directly from stereo_images[0] */
            if (res == VK_SUCCESS && setup_barrier_resources(sd, sc)) {
                sc->present_mode  = req;
                sc->dxgi_mode     = false;
                sc->stereo_active = true;
                *pSwapchain       = (VkSwapchainKHR)(uintptr_t)sc;
                sc->app_handle    = *pSwapchain;
                STEREO_LOG(
                    "[CREATE SC GPU FINAL] sc=%p app=%p real=%p active=%d count=%u",
                    sc,
                    sc->app_handle,
                    sc->real_swapchain,
                    (int)sc->stereo_active,
                    sd->swapchain_count);
                if (!old_sc)
                    sd->swapchain_count++;
                STEREO_LOG(
                    "[CREATE SC GPU FINAL] sc=%p app=%p real=%p active=%d count=%u",
                    sc,
                    sc->app_handle,
                    sc->real_swapchain,
                    (int)sc->stereo_active,
                    sd->swapchain_count);
                sd->stereo_w = app_w;
                sd->stereo_h = app_h;
                STEREO_LOG(
                    "[CREATE SC GPU] sc=%p app_handle=%p returned=%p",
                    sc,
                    sc->app_handle,
                    *pSwapchain);
                STEREO_LOG(
                    "GPU-blit stereo swapchain (mode=%d): %ux%u  handle=%p",
                    (int)req,
                    app_w,
                    app_h,
                    (void*)*pSwapchain);
                return VK_SUCCESS;
            }
            /* GPU compose init failed — fall to passthrough */
            STEREO_LOG("[DESTROY SC] before gpu_compose_sc_destroy");
            gpu_compose_sc_destroy(sd, sc);
            STEREO_LOG("[DESTROY SC] after gpu_compose_sc_destroy");
            if (sc->real_swapchain) {
                STEREO_LOG(
                    "[COMPOSE DESTROY] (swapchain.c) destroying=%p",
                    sc->real_swapchain);
                STEREO_LOG(
                    "[COMPOSE DESTROY] sc=%p app=%p real=%p",
                    sc,
                    sc->app_handle,
                    sc->real_swapchain);
                sd->real.DestroySwapchainKHR(sd->real_device, sc->real_swapchain, NULL);
                STEREO_LOG(
                    "[COMPOSE DESTROY] (swapchain.c) destroyed=%p",
                    sc->real_swapchain);
                sc->real_swapchain = VK_NULL_HANDLE;
            }
        }
    }

passthrough:
    STEREO_ERR("All stereo modes failed — passthrough");
    STEREO_LOG(
        "[PASSTHROUGH] entering real CreateSwapchainKHR old=%p",
        pCreateInfo->oldSwapchain);
    sc->stereo_active = false;
    STEREO_LOG(
        "[PASSTHROUGH] calling real CreateSwapchainKHR");

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;

    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
    {
        StereoSwapchain *old_sc =
            stereo_swapchain_lookup(sd, pCreateInfo->oldSwapchain);
        STEREO_LOG(
            "[PASSTHROUGH LOOKUP] old=%p old_sc=%p real=%p",
            pCreateInfo->oldSwapchain,
            old_sc,
            old_sc ? old_sc->real_swapchain : VK_NULL_HANDLE);

        if (old_sc)
        {
            ci.oldSwapchain = old_sc->real_swapchain;

            STEREO_LOG(
                "[PASSTHROUGH] translated old swapchain %p -> %p",
                pCreateInfo->oldSwapchain,
                ci.oldSwapchain);
        }
        else
        {
            ci.oldSwapchain = pCreateInfo->oldSwapchain;

            STEREO_LOG(
                "[PASSTHROUGH] forwarding unknown old swapchain %p",
                ci.oldSwapchain);
        }

    } 
    VkResult res =
        sd->real.CreateSwapchainKHR(
            sd->real_device,
            &ci,
            pAllocator,
            pSwapchain);
    STEREO_LOG(
        "[PASSTHROUGH] returned %d swapchain=%p",
        (int)res,
        res == VK_SUCCESS ? *pSwapchain : VK_NULL_HANDLE);
    if (res == VK_SUCCESS) {
        sc->real_swapchain = *pSwapchain;
        sc->app_handle     = *pSwapchain;
        sc->stereo_active  = false;
        if (pCreateInfo->oldSwapchain == VK_NULL_HANDLE)
            sd->swapchain_count++;
    }
    return res;
}

/* ── vkDestroySwapchainKHR ──────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *pAllocator)
{
    STEREO_LOG(
        "[DESTROY SC ENTRY] swapchain=%p",
        swapchain);
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;
    STEREO_LOG(
        "[DESTROY SC START] count=%u",
        sd->swapchain_count);

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);

    if (sc && sc->resize_reused)
    {
        STEREO_LOG(
            "[DESTROY SC] ignoring recycled resize swapchain app=%p sc=%p",
            swapchain,
            sc);
    
        sc->resize_reused = false;
        return;
    }

    STEREO_LOG(
        "[DESTROY SC] present_mode=%d active=%d app=%p real=%p",
        sc ? (int)sc->present_mode : -1,
        sc ? (int)sc->stereo_active : -1,
        sc ? sc->app_handle : VK_NULL_HANDLE,
        sc ? sc->real_swapchain : VK_NULL_HANDLE);
    STEREO_LOG(
        "[DESTROY SC LOOKUP] app=%p sc=%p",
        swapchain,
        sc);

    if (sc) {
        STEREO_LOG(
            "[DESTROY SC] stereo_active=%d",
            sc ? (int)sc->stereo_active : -1);

        STEREO_LOG(
            "[DESTROY SC] image_count=%u stereo_images=%p stereo_views=%p",
            sc->image_count,
            sc->stereo_images,
            sc->stereo_views_arr);

        for (uint32_t i = 0; i < sc->image_count; i++)
        {
            STEREO_LOG("[DESTROY SC] image %u", i);

            if (sc->stereo_views_arr && sc->stereo_views_arr[i])
            {
                STEREO_LOG("[DESTROY SC] destroy imageview %u", i);
                stereo_DestroyImageView(
                    device,
                    sc->stereo_views_arr[i],
                    NULL);
            }

            if (sc->stereo_images && sc->stereo_images[i])
            {
                STEREO_LOG("[DESTROY SC] destroy image %u", i);
                STEREO_LOG(
                    "[DESTROY IMAGE TRACKED?] image=%p",
                    sc->stereo_images[i]);
                remove_tracked_image(
                    sd->intercepted_depth,
                    &sd->intercepted_depth_count,
                    sc->stereo_images[i]);
                remove_tracked_image(
                    sd->intercepted_color,
                    &sd->intercepted_color_count,
                    sc->stereo_images[i]);
                sd->real.DestroyImage(
                    sd->real_device,
                    sc->stereo_images[i],
                    NULL);
            }

            if (sc->stereo_memory && sc->stereo_memory[i])
            {
                STEREO_LOG("[DESTROY SC] free memory %u", i);
                sd->real.FreeMemory(
                    sd->real_device,
                    sc->stereo_memory[i],
                    NULL);
            }
            if (sc->barrier_fences && sc->barrier_fences[i])
                sd->real.DestroyFence(sd->real_device, sc->barrier_fences[i], NULL);
        }
        free(sc->stereo_views_arr);
        free(sc->stereo_images);
        STEREO_LOG(
            "[IMAGE TRACK COUNTS] depth=%u color=%u",
            sd->intercepted_depth_count,
            sd->intercepted_color_count);
        free(sc->stereo_memory);
        free(sc->barrier_cmds);
        free(sc->barrier_fences);

        sc->stereo_views_arr = NULL;
        sc->stereo_images    = NULL;
        sc->stereo_memory    = NULL;
        sc->barrier_cmds     = NULL;
        sc->barrier_fences   = NULL;
        sc->image_count      = 0;

        if (sc->barrier_pool)
            sd->real.DestroyCommandPool(sd->real_device, sc->barrier_pool, NULL);
        sc->barrier_pool = VK_NULL_HANDLE;
        STEREO_LOG(
            "[NV3D] QueuePresent mode=%d sc=%p",
            (int)sc->present_mode,
            sc);
        if (sc->present_mode == STEREO_PRESENT_NV3DLIB)
            nv3d_destroy(sd);

        STEREO_LOG("[DESTROY SC] before gpu_compose_sc_destroy");
        gpu_compose_sc_destroy(sd, sc);     /* semaphores + comp_sc_images array */
        STEREO_LOG("[DESTROY SC] after gpu_compose_sc_destroy");
        STEREO_LOG("[DESTROY SC] before alt_cpu_staging_destroy");
        alt_cpu_staging_destroy(sd, sc);    /* DX9 CPU staging (no-op if unused) */
        STEREO_LOG("[DESTROY SC] after alt_cpu_staging_destroy");
        STEREO_LOG("[DESTROY SC] before dxgi_sc_destroy");
        dxgi_sc_destroy(sc);
        STEREO_LOG("[DESTROY SC] after dxgi_sc_destroy");

        /* real_swapchain: GPU compose output SC or passthrough SC */
        if (sc->real_swapchain)
        {
            STEREO_LOG(
                "[DESTROY SC] app=%p sc=%p real=%p",
                swapchain,
                sc,
                sc->real_swapchain);
            STEREO_LOG(
                "[COMPOSE DESTROY] (swapchain.c) destroying=%p",
                sc->real_swapchain);
            sd->real.DestroySwapchainKHR(
                sd->real_device,
                sc->real_swapchain,
                pAllocator);
            STEREO_LOG(
                "[COMPOSE DESTROY] (swapchain.c) destroyed=%p",
                sc->real_swapchain);
            sc->real_swapchain = VK_NULL_HANDLE;
        }

        STEREO_LOG(
            "[DESTROY SC] keeping slot alive sc=%p",
            sc);

        /* leave structure in table */
        sc->stereo_active = false;

    } else {
    STEREO_LOG(
        "[DESTROY SC PASSTHROUGH] BEFORE destroy swapchain=%p",
        swapchain);

    STEREO_LOG(
        "[DESTROY SC PASSTHROUGH] device=%p",
        sd->real_device);

    STEREO_LOG(
        "[DESTROY SC PASSTHROUGH] calling real destroy device=%p swapchain=%p",
        sd->real_device,
        swapchain);

    sd->real.DestroySwapchainKHR(
        sd->real_device,
        swapchain,
        pAllocator);

    STEREO_LOG(
        "[DESTROY SC PASSTHROUGH] real destroy returned");

    STEREO_LOG(
        "[DESTROY SC PASSTHROUGH] AFTER destroy swapchain=%p",
        swapchain);
    }
    STEREO_LOG(
        "[DESTROY SC END] count=%u",
        sd->swapchain_count);
}

/* ── vkGetSwapchainImagesKHR ────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t *pCount,
    VkImage *pImages)
{
    STEREO_LOG(
        "GetSwapchainImagesKHR swapchain=%p count_ptr=%p images_ptr=%p",
        swapchain,
        pCount,
        pImages);
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    STEREO_LOG(
        "[GET IMAGES] sc=%p",
        sc);

    STEREO_LOG(
        "[GET IMAGES] stereo_active addr=%p",
        &sc->stereo_active);
    STEREO_LOG(
        "[GET IMAGES LOOKUP] app=%p sc=%p real=%p active=%d",
        swapchain,
        sc,
        sc ? sc->real_swapchain : VK_NULL_HANDLE,
        sc ? sc->stereo_active : -1);
    if (!sc || !sc->stereo_active)
    {
        STEREO_LOG(
            "[GET IMAGES PASSTHROUGH] sc=%p active=%d real=%p",
            sc,
            sc ? (int)sc->stereo_active : -1,
            sc ? sc->real_swapchain : VK_NULL_HANDLE);

        if (sc && sc->real_swapchain == VK_NULL_HANDLE)
        {
            STEREO_ERR(
                "[GET IMAGES] called on destroyed stereo swapchain");
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        VkSwapchainKHR real =
            sc ? sc->real_swapchain : swapchain;

        return sd->real.GetSwapchainImagesKHR(
            sd->real_device,
            real,
            pCount,
            pImages);
    }
    STEREO_LOG(
        "GetSwapchainImagesKHR stereo=%d image_count=%u",
        sc ? sc->stereo_active : 0,
        sc ? sc->image_count : 0);
    if (!pImages)
    {
        STEREO_LOG(
            "[NV3D TEST] count query image_count=%u",
            sc->image_count);

        *pCount = sc->image_count;
        return VK_SUCCESS;
    }
    uint32_t copy = (*pCount < sc->image_count) ? *pCount : sc->image_count;
    STEREO_LOG(
        "GetSwapchainImagesKHR returning %u images stereo_images=%p",
        copy,
        sc->stereo_images);
    for (uint32_t i = 0; i < copy; i++)
    {
        pImages[i] = sc->stereo_images[i];

        STEREO_LOG(
            "[NV3D TEST] image[%u]=%p",
            i,
            (void*)pImages[i]);
    }
    *pCount = copy;
    return (copy < sc->image_count) ? VK_INCOMPLETE : VK_SUCCESS;

}

/* ── vkAcquireNextImageKHR ───────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                            uint64_t timeout, VkSemaphore semaphore,
                            VkFence fence, uint32_t *pImageIndex)
{

    StereoDevice *sd = stereo_device_from_handle(device);
    STEREO_LOG(
        "[NV3D] acquire gfx_queue=%p",
        sd ? sd->gfx_queue : NULL);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("stereo_AcquireNextImageKHR: sc=%p", (void*)swapchain);

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);

    STEREO_LOG(
        "[ACQUIRE LOOKUP] app=%p sc=%p real=%p active=%d",
        swapchain,
        sc,
        sc ? sc->real_swapchain : VK_NULL_HANDLE,
        sc ? sc->stereo_active : -1);
    STEREO_LOG(
        "stereo_AcquireNextImageKHR: sc=%p mode=%d real_sc=%p",
        sc,
        sc ? (int)sc->present_mode : -1,
        sc ? (void*)sc->real_swapchain : 0);

    if (sc &&
        sc->present_mode == STEREO_PRESENT_NV3DLIB)
    {
        STEREO_LOG("[NV3D] AcquireNextImageKHR begin");

    if (semaphore != VK_NULL_HANDLE)
    {
        VkSubmitInfo sig = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = 1, 
            .pSignalSemaphores = &semaphore,
        };

        STEREO_LOG(
            "[NV3D] signaling acquire semaphore %p",
            semaphore);

        sd->real.QueueSubmit(
            sd->gfx_queue,
            1,
            &sig,
            VK_NULL_HANDLE);
    }

    if (fence != VK_NULL_HANDLE)
    {
        sd->real.ResetFences(
            sd->real_device,
            1,
            &fence);

        sd->real.QueueSubmit(
            sd->gfx_queue,
            0,
            NULL,
            fence);

        STEREO_LOG(
            "[NV3D] signaling acquire fence %p",
            fence);
    }

    if (pImageIndex)
        *pImageIndex = 0;

    STEREO_LOG(
        "[NV3D] AcquireNextImageKHR return success");

    return VK_SUCCESS;
    }

    if (!sc || !sc->stereo_active)
    {
        STEREO_LOG(
            "[ACQUIRE PASSTHROUGH] sc=%p active=%d real=%p",
            sc,
            sc ? (int)sc->stereo_active : -1,
            sc ? sc->real_swapchain : VK_NULL_HANDLE);

        if (sc && sc->real_swapchain == VK_NULL_HANDLE)
        {
            STEREO_ERR(
                "[ACQUIRE] called on destroyed stereo swapchain");
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        VkSwapchainKHR real =
            sc ? sc->real_swapchain : swapchain;

        return sd->real.AcquireNextImageKHR(
            sd->real_device,
            real,
            timeout,
            semaphore,
            fence,
            pImageIndex);
    }

    /* Wait for the previous frame's GPU work (DXGI barrier or GPU blit) to
     * complete before allowing the app to render into stereo_images[0] again.
     * barrier_fences[0] starts SIGNALED so the very first acquire never blocks. */
    if (sc->barrier_fences && sc->barrier_fences[0]) {
        VkResult wres = sd->real.WaitForFences(
            sd->real_device, 1, &sc->barrier_fences[0], VK_TRUE, timeout);
        if (wres != VK_SUCCESS) return wres;
    }

    /* Signal app semaphore/fence so it knows the image is available */
    if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
        VkSubmitInfo sig = {
            .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = (semaphore != VK_NULL_HANDLE) ? 1 : 0,
            .pSignalSemaphores    = &semaphore,
        };
        if (sd->gfx_queue) sd->real.QueueSubmit(sd->gfx_queue, 1, &sig, fence);
    }
    *pImageIndex = 0;
    return VK_SUCCESS;
}

/* ── vkQueuePresentKHR ───────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    STEREO_LOG(
        "[NV3D] QueuePresentKHR queue=%p swapchains=%u",
        queue,
        pPresentInfo ?
            pPresentInfo->swapchainCount : 0);
    STEREO_LOG("stereo_QueuePresentKHR: queue=%p swapchainCount=%u",
               (void*)queue, pPresentInfo ? pPresentInfo->swapchainCount : 0);
    extern StereoDevice g_devices[];
    extern uint32_t     g_device_count;

    StereoDevice    *sd = NULL;
    StereoSwapchain *sc = NULL;
    for (uint32_t d = 0; d < g_device_count && !sd; d++) {
        for (uint32_t p = 0; p < pPresentInfo->swapchainCount; p++) {
            StereoSwapchain *found = stereo_swapchain_lookup(
                &g_devices[d], pPresentInfo->pSwapchains[p]);
            if (found) { sd = &g_devices[d]; sc = found; break; }
        }
    }

    if (!sd || !sc || !sd->stereo.enabled || !sc->stereo_active) {
        StereoDevice *fwd = sd ? sd : (g_device_count > 0 ? &g_devices[0] : NULL);
        if (!fwd) return VK_ERROR_DEVICE_LOST;
        return fwd->real.QueuePresentKHR(queue, pPresentInfo);
    }

    hotkeys_poll(sd);

    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        StereoSwapchain *sc_i = stereo_swapchain_lookup(sd, pPresentInfo->pSwapchains[i]);
        if (!sc_i || !sc_i->stereo_active) continue;

        uint32_t           wcount = (i == 0) ? pPresentInfo->waitSemaphoreCount : 0;
        const VkSemaphore *wsems  = (i == 0) ? pPresentInfo->pWaitSemaphores    : NULL;

        VkResult pr;
        switch (sc_i->present_mode) {
        case STEREO_PRESENT_DXGI:
            pr = stereo_dxgi_present(sd, queue, sc_i, 0, wcount, wsems);
            break;
        case STEREO_PRESENT_DX9:
            pr = dx9_present(sd, sc_i, queue, wcount, wsems);
            break;
        case STEREO_PRESENT_NV3DLIB:
            pr = nv3d_present(
                sd,
                sc_i,
                queue,
                wcount,
                wsems);
            break;
        case STEREO_PRESENT_SBS:
        case STEREO_PRESENT_TAB:
        case STEREO_PRESENT_INTERLACED:
            /* GPU blit compose — no CPU readback, no GDI */
            pr = gpu_compose_present(sd, sc_i, queue, wcount, wsems);
            break;
        default:
            pr = VK_SUCCESS;
            break;
        }
        if (pr != VK_SUCCESS) {
            STEREO_ERR("Present (mode=%d) failed: %d", (int)sc_i->present_mode, pr);
            result = pr;
        }
    }
    return result;
}

/* ── stereo_CreateImage / stereo_CreateImageView ────────────────────────── */

static bool is_depth_format(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:   case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default: return false;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* Upgrade images used as color/depth attachments (G-buffer and scene depth)
     * so multiview pipelines and framebuffers align.  This uses usage flags
     * rather than strictly matching the swapchain extent, preventing the
     * per-pass MV mismatch that caused overlapping geometry.
     */
    bool base = sd->stereo.enabled && sd->stereo.multiview
        && pCreateInfo
        && pCreateInfo->imageType   == VK_IMAGE_TYPE_2D
        && pCreateInfo->arrayLayers == 1
        && pCreateInfo->samples     == VK_SAMPLE_COUNT_1_BIT;

    /* Depth/stencil attachments — upgraded for multiview depth per eye */
    bool intercept_depth = base
        && (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    /* Color attachments that are also sampled (render-to-texture G-buffers,
     * shadow-color, lighting output, post-fx targets).  mipLevels==1 and
     * extent > 1x1 to avoid upgrading LUTs or procedural textures.
     * Also intercept non-sampled color attachments (needed so every
     * framebuffer attachment has 2 layers for the multiview render pass). */
    bool intercept_color = base
        && pCreateInfo->mipLevels == 1
        && pCreateInfo->extent.width  > 1
        && pCreateInfo->extent.height > 1
        && (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (base &&
        (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
        !(pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        STEREO_LOG(
            "stereo_CreateImage: depth-only attachment left mono [%ux%u]",
            pCreateInfo->extent.width,
            pCreateInfo->extent.height);
    }

    if (!intercept_depth && !intercept_color)
        return sd->real.CreateImage(sd->real_device, pCreateInfo, pAllocator, pImage);

    VkImageCreateInfo modified = *pCreateInfo;
    modified.arrayLayers = 2;
    VkResult res = sd->real.CreateImage(sd->real_device, &modified, pAllocator, pImage);
    if (res == VK_SUCCESS) {
        STEREO_LOG(
            "[CREATE IMAGE RESULT] image=%p usage=0x%08X layers=%u",
            *pImage,
            pCreateInfo->usage,
            pCreateInfo->arrayLayers);
        if (intercept_depth &&
            sd->intercepted_depth_count < MAX_DEPTH_IMAGES)
        {
            sd->intercepted_depth[
                sd->intercepted_depth_count++] = *pImage;

            STEREO_LOG(
                "[DEPTH TRACK ADD] image=%p count=%u",
                *pImage,
                sd->intercepted_depth_count);
        }
        else if (intercept_depth)
        {
            STEREO_LOG(
                "[DEPTH TRACK FULL] image=%p count=%u max=%u",
                *pImage,
                sd->intercepted_depth_count,
                MAX_DEPTH_IMAGES);
        }
        if (intercept_color &&
            sd->intercepted_color_count < MAX_COLOR_IMAGES)
        {
            sd->intercepted_color[
                sd->intercepted_color_count++] = *pImage;

            STEREO_LOG(
                "[COLOR TRACK ADD] image=%p count=%u",
                *pImage,
                sd->intercepted_color_count);
        }
        else if (intercept_color)
        {
            STEREO_LOG(
                "[COLOR TRACK FULL] image=%p count=%u max=%u",
                *pImage,
                sd->intercepted_color_count,
                MAX_COLOR_IMAGES);
        }
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    if (!sd->stereo.multiview)
        return sd->real.CreateImageView(sd->real_device, pCreateInfo, pAllocator, pView);

    STEREO_LOG(
        "[VIEW CREATE RAW] image=%p viewType=%u layers=%u",
        pCreateInfo->image,
        pCreateInfo->viewType,
        pCreateInfo->subresourceRange.layerCount);

    bool needs_upgrade = false;
    for (uint32_t si = 0; si < sd->swapchain_count && !needs_upgrade; si++) {
        StereoSwapchain *scc = &sd->swapchains[si];
        if (!scc->stereo_active || !scc->stereo_images) continue;
        for (uint32_t ii = 0; ii < scc->image_count && !needs_upgrade; ii++)
            if (scc->stereo_images[ii] == pCreateInfo->image) needs_upgrade = true;
    }
    STEREO_LOG(
        "[VIEW LOOKUP] image=%p depth_count=%u color_count=%u",
        pCreateInfo->image,
        sd->intercepted_depth_count,
        sd->intercepted_color_count);
    uint32_t depth_matches = 0;
    uint32_t color_matches = 0;
    for (uint32_t i = 0; i < sd->intercepted_depth_count && !needs_upgrade; i++)
    {
        if (sd->intercepted_depth[i] == pCreateInfo->image)
        {
            depth_matches++;
            needs_upgrade = true;
        }
    }
    for (uint32_t i = 0; i < sd->intercepted_color_count && !needs_upgrade; i++)
    {
        if (sd->intercepted_color[i] == pCreateInfo->image)
        {
            color_matches++;
            needs_upgrade = true;
        }
    }
    STEREO_LOG(
        "[VIEW DECISION] image=%p needs_upgrade=%d depth_matches=%u color_matches=%u",
        pCreateInfo->image,
        (int)needs_upgrade,
        depth_matches,
        color_matches);
     if (!needs_upgrade)
        {
            STEREO_LOG(
                "[VIEW PASSTHROUGH] image=%p",
                pCreateInfo->image);
         return sd->real.CreateImageView(
             sd->real_device,
             pCreateInfo,
             pAllocator,
             pView);
        }

    STEREO_LOG(
        "[VIEW CREATE] image=%p layers=%u viewType=%u",
        (void*)(uintptr_t)pCreateInfo->image,
        pCreateInfo->subresourceRange.layerCount,
        pCreateInfo->viewType);
    VkImageViewCreateInfo upgraded = *pCreateInfo;
    if (upgraded.viewType == VK_IMAGE_VIEW_TYPE_2D)
        upgraded.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (upgraded.subresourceRange.layerCount < 2)
        upgraded.subresourceRange.layerCount = 2;
    STEREO_LOG(
        "[VIEW UPGRADED] image=%p oldType=%u newType=%u oldLayers=%u newLayers=%u",
        (void*)(uintptr_t)pCreateInfo->image,
        pCreateInfo->viewType,
        upgraded.viewType,
        pCreateInfo->subresourceRange.layerCount,
        upgraded.subresourceRange.layerCount);
    STEREO_LOG("stereo_CreateImageView: upgraded %p → 2D_ARRAY/layerCount=2 [multiview=1]",
               (void*)(uintptr_t)pCreateInfo->image);
    VkResult _r = sd->real.CreateImageView(sd->real_device, &upgraded, pAllocator, pView);
    /* Track upgraded views for framebuffer multiview detection */
    if (_r == VK_SUCCESS &&
        sd->upgraded_view_count < MAX_UPGRADED_VIEWS)
    {
        sd->upgraded_views[sd->upgraded_view_count++] = *pView;

        STEREO_LOG(
            "[VIEW TRACK ADD] view=%p count=%u",
            *pView,
            sd->upgraded_view_count);
    }
    STEREO_LOG(
        "[VIEW TRACKED] view=%p count=%u",
        *pView,
        sd->upgraded_view_count);
    return _r;
}

VKAPI_ATTR void VKAPI_CALL
stereo_DestroyImageView(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks *pAllocator)
{
    StereoDevice *sd = stereo_device_from_handle(device);

    if (!sd)
        return;

    for (uint32_t i = 0;
         i < sd->upgraded_view_count;
         i++)
    {
        if (sd->upgraded_views[i] == imageView)
        {
            STEREO_LOG(
                "[VIEW TRACK REMOVE] view=%p slot=%u",
                imageView,
                i);

            memmove(
                &sd->upgraded_views[i],
                &sd->upgraded_views[i + 1],
                (sd->upgraded_view_count - i - 1) *
                    sizeof(VkImageView));

            sd->upgraded_view_count--;

            STEREO_LOG(
                "[VIEW TRACK COUNT] count=%u",
                sd->upgraded_view_count);

            break;
        }
    }

    sd->real.DestroyImageView(
        sd->real_device,
        imageView,
        pAllocator);
}