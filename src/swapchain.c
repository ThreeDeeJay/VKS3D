/*
 * swapchain.c — DXGI 1.2 stereo swap chain (3D Vision) + passthrough
 *
 * Strategy (DXGI stereo mode)
 * ───────────────────────────
 * When stereo is enabled we bypass the Vulkan swap chain entirely for
 * presentation and use DXGI 1.2's Stereo=TRUE Texture2DArray[2] instead.
 *
 *   slice 0 = left  eye
 *   slice 1 = right eye
 *
 * Pipeline per frame:
 *   1.  App calls AcquireNextImageKHR  → round-robin index, no-op submit
 *       signals the app's semaphore/fence.
 *   2.  App renders into stereo_images[idx] (VkImage, arrayLayers=2,
 *       layer 0 = left, layer 1 = right) via multiview render pass.
 *   3.  App calls QueuePresentKHR → VKS3D intercepts.
 *   4.  VKS3D records + submits a staging command buffer that copies
 *       both layers of stereo_images[idx] into stage_buf[idx]
 *       (host-visible buffer, 2 × W×H×4 bytes).
 *   5.  CPU waits for the staging fence → stage_mapped[idx] has the pixels.
 *   6.  dxgi_present_frame() uploads pixels to D3D11 textures and calls
 *       IDXGISwapChain::Present.
 *
 * Passthrough (stereo disabled or DXGI init failed): every call is forwarded
 * unchanged to the real Vulkan ICD.
 *
 * Side-By-Side (SBS) mode is deliberately NOT implemented here.  NVIDIA
 * driver 426.06 does not auto-detect SBS in Vulkan; DXGI 1.2 stereo is the
 * only supported path for 3D Vision output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"

/* ── Helper: find suitable memory type ──────────────────────────────────── */
static uint32_t find_memory_type(
    StereoDevice *sd,
    uint32_t     type_bits,
    VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(sd->real_physdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/* ── Allocate one single-layer render target (W×H) ───────────────────────
 * NOTE: We do NOT use arrayLayers=2 + multiview here.  Multiview requires
 * ALL framebuffer attachments (including depth/stencil) to be array images
 * covering all views.  We do not intercept vkCreateImage, so the app's
 * depth buffer remains a single-layer image.  Using viewMask=0x3 with a
 * single-layer depth buffer causes an invalid GPU access → device lost.
 *
 * Instead we render once (mono), copy the result to BOTH DXGI eye slices.
 * Both eyes see the same content — 3D Vision engages without stereo
 * separation.  True per-eye rendering can be added later by also
 * intercepting depth image creation.
 * ───────────────────────────────────────────────────────────────────────── */
static VkResult alloc_stereo_image(
    StereoDevice   *sd,
    uint32_t        w,
    uint32_t        h,
    VkFormat        fmt,
    VkImage        *out_image,
    VkDeviceMemory *out_mem)
{
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = {w, h, 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,   /* single layer — mono render, copied to both eyes */
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = sd->real.CreateImage(sd->real_device, &ici, NULL, out_image);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements mr;
    sd->real.GetImageMemoryRequirements(sd->real_device, *out_image, &mr);

    uint32_t mt = find_memory_type(sd, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

    return sd->real.BindImageMemory(sd->real_device, *out_image, *out_mem, 0);
}

/* ── Allocate host-visible staging buffer (2 eyes × W×H×4 bytes) ─────── */
static VkResult alloc_stage_buf(
    StereoDevice   *sd,
    uint32_t        w,
    uint32_t        h,
    VkBuffer       *out_buf,
    VkDeviceMemory *out_mem,
    void          **out_mapped)
{
    VkDeviceSize size = (VkDeviceSize)w * h * 4;  /* one frame, copied to both eyes */

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult res = sd->real.CreateBuffer(sd->real_device, &bci, NULL, out_buf);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements mr;
    sd->real.GetBufferMemoryRequirements(sd->real_device, *out_buf, &mr);

    uint32_t mt = find_memory_type(sd, mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        sd->real.DestroyBuffer(sd->real_device, *out_buf, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, out_mem);
    if (res != VK_SUCCESS) {
        sd->real.DestroyBuffer(sd->real_device, *out_buf, NULL);
        return res;
    }

    res = sd->real.BindBufferMemory(sd->real_device, *out_buf, *out_mem, 0);
    if (res != VK_SUCCESS) return res;

    return sd->real.MapMemory(sd->real_device, *out_mem, 0, VK_WHOLE_SIZE, 0, out_mapped);
}

/* ── vkCreateSwapchainKHR ──────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateSwapchainKHR(
    VkDevice                          device,
    const VkSwapchainCreateInfoKHR   *pCreateInfo,
    const VkAllocationCallbacks      *pAllocator,
    VkSwapchainKHR                   *pSwapchain)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* ── Passthrough: stereo disabled or slot exhausted ─────────────── */
    if (!sd->stereo.enabled || sd->swapchain_count >= MAX_SWAPCHAINS) {
        return sd->real.CreateSwapchainKHR(
            sd->real_device, pCreateInfo, pAllocator, pSwapchain);
    }

    uint32_t app_w = pCreateInfo->imageExtent.width;
    uint32_t app_h = pCreateInfo->imageExtent.height;

    /* ── Register swapchain slot ─────────────────────────────────────── */
    StereoSwapchain *sc = &sd->swapchains[sd->swapchain_count];
    memset(sc, 0, sizeof(*sc));
    sc->device     = sd->real_device;
    sc->app_width  = app_w;
    sc->app_height = app_h;
    sc->format     = pCreateInfo->imageFormat;
    sc->hwnd       = stereo_si_hwnd_for_surface(sd->si, pCreateInfo->surface);

    /* ── Try DXGI 1.2 stereo mode ────────────────────────────────────── */
    bool dxgi_ok = false;
    if (sc->hwnd && dxgi_device_init(sd)) {
        if (dxgi_sc_create(sd, sc) && dxgi_tex_create(sd, sc))
            dxgi_ok = true;
        else
            dxgi_sc_destroy(sc);
    }

    if (!dxgi_ok) {
        STEREO_ERR("DXGI stereo init failed — passing through unmodified");
        sc->stereo_active = false;
        VkResult res = sd->real.CreateSwapchainKHR(
            sd->real_device, pCreateInfo, pAllocator, pSwapchain);
        if (res == VK_SUCCESS) {
            sc->real_swapchain = *pSwapchain;
            sc->app_handle     = *pSwapchain;
            sd->swapchain_count++;
        }
        return res;
    }

    /* ── DXGI mode: allocate stereo render targets ───────────────────── */
    /* Use a fixed image count (3-buffered) since we don't have a real  */
    /* Vulkan swap chain to query.                                       */
    sc->image_count = 3;

    sc->stereo_images    = calloc(sc->image_count, sizeof(VkImage));
    sc->stereo_memory    = calloc(sc->image_count, sizeof(VkDeviceMemory));
    sc->stereo_views_arr = calloc(sc->image_count, sizeof(VkImageView));
    sc->stage_buf        = calloc(sc->image_count, sizeof(VkBuffer));
    sc->stage_mem        = calloc(sc->image_count, sizeof(VkDeviceMemory));
    sc->stage_mapped     = calloc(sc->image_count, sizeof(void*));
    sc->stage_cmds       = calloc(sc->image_count, sizeof(VkCommandBuffer));
    sc->stage_fences     = calloc(sc->image_count, sizeof(VkFence));

    if (!sc->stereo_images || !sc->stereo_memory || !sc->stereo_views_arr ||
        !sc->stage_buf || !sc->stage_mem || !sc->stage_mapped ||
        !sc->stage_cmds || !sc->stage_fences) {
        dxgi_sc_destroy(sc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkResult res = alloc_stereo_image(sd, app_w, app_h, sc->format,
                                           &sc->stereo_images[i],
                                           &sc->stereo_memory[i]);
        if (res != VK_SUCCESS) {
            STEREO_ERR("Failed to allocate stereo image %u: %d", i, res);
            dxgi_sc_destroy(sc);
            /* Free already-allocated images */
            for (uint32_t j = 0; j < i; j++) {
                sd->real.DestroyImage(sd->real_device, sc->stereo_images[j], NULL);
                sd->real.FreeMemory(sd->real_device, sc->stereo_memory[j], NULL);
            }
            return res;
        }

        /* 2D view for single-layer render target */
        VkImageViewCreateInfo vci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = sc->stereo_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = sc->format,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        res = sd->real.CreateImageView(sd->real_device, &vci, NULL,
                                        &sc->stereo_views_arr[i]);
        if (res != VK_SUCCESS) return res;

        /* Host-visible staging buffer for CPU readback */
        res = alloc_stage_buf(sd, app_w, app_h,
                               &sc->stage_buf[i], &sc->stage_mem[i],
                               &sc->stage_mapped[i]);
        if (res != VK_SUCCESS) {
            STEREO_ERR("Failed to allocate staging buffer %u: %d", i, res);
            return res;
        }
    }

    /* ── Command pool for staging submits ────────────────────────────── */
    VkCommandPoolCreateInfo cpci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = sd->gfx_qf,
    };
    VkResult res = sd->real.CreateCommandPool(
        sd->real_device, &cpci, NULL, &sc->stage_pool);
    if (res != VK_SUCCESS) return res;

    VkCommandBufferAllocateInfo cbai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = sc->stage_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = sc->image_count,
    };
    res = sd->real.AllocateCommandBuffers(sd->real_device, &cbai, sc->stage_cmds);
    if (res != VK_SUCCESS) return res;

    /* Create fences in SIGNALED state so the first AcquireNextImage
     * doesn't stall waiting for a submit that never happened.         */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        res = sd->real.CreateFence(sd->real_device, &fci, NULL, &sc->stage_fences[i]);
        if (res != VK_SUCCESS) return res;
    }

    /* ── Set fake swapchain handle returned to app ───────────────────── */
    sc->dxgi_mode    = true;
    sc->stereo_active = true;
    sc->real_swapchain = VK_NULL_HANDLE;  /* no Vulkan swap chain in DXGI mode */
    sc->app_handle   = (VkSwapchainKHR)(uintptr_t)sc;
    *pSwapchain      = sc->app_handle;

    sd->swapchain_count++;
    STEREO_LOG("DXGI stereo swapchain: %ux%u  %u images  handle=%p",
               app_w, app_h, sc->image_count, (void*)*pSwapchain);
    return VK_SUCCESS;
}

/* ── vkDestroySwapchainKHR ──────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroySwapchainKHR(
    VkDevice                         device,
    VkSwapchainKHR                   swapchain,
    const VkAllocationCallbacks     *pAllocator)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (sc) {
        /* Destroy per-image resources */
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->stereo_views_arr && sc->stereo_views_arr[i])
                sd->real.DestroyImageView(sd->real_device, sc->stereo_views_arr[i], NULL);
            if (sc->stereo_images && sc->stereo_images[i])
                sd->real.DestroyImage(sd->real_device, sc->stereo_images[i], NULL);
            if (sc->stereo_memory && sc->stereo_memory[i])
                sd->real.FreeMemory(sd->real_device, sc->stereo_memory[i], NULL);

            if (sc->stage_buf && sc->stage_buf[i]) {
                if (sc->stage_mem && sc->stage_mem[i])
                    sd->real.UnmapMemory(sd->real_device, sc->stage_mem[i]);
                sd->real.DestroyBuffer(sd->real_device, sc->stage_buf[i], NULL);
            }
            if (sc->stage_mem && sc->stage_mem[i])
                sd->real.FreeMemory(sd->real_device, sc->stage_mem[i], NULL);
            if (sc->stage_fences && sc->stage_fences[i])
                sd->real.DestroyFence(sd->real_device, sc->stage_fences[i], NULL);
        }
        free(sc->stereo_views_arr);
        free(sc->stereo_images);
        free(sc->stereo_memory);
        free(sc->stage_buf);
        free(sc->stage_mem);
        free(sc->stage_mapped);
        free(sc->stage_cmds);
        free(sc->stage_fences);

        if (sc->stage_pool)
            sd->real.DestroyCommandPool(sd->real_device, sc->stage_pool, NULL);

        /* DXGI resources */
        dxgi_sc_destroy(sc);

        /* Vulkan real swapchain (passthrough mode only) */
        if (sc->real_swapchain)
            sd->real.DestroySwapchainKHR(sd->real_device, sc->real_swapchain, pAllocator);

        /* Remove from registry */
        uint32_t idx = (uint32_t)(sc - sd->swapchains);
        if (idx + 1 < sd->swapchain_count)
            memmove(&sd->swapchains[idx], &sd->swapchains[idx + 1],
                    (sd->swapchain_count - idx - 1) * sizeof(StereoSwapchain));
        sd->swapchain_count--;
    } else {
        /* Not our swapchain — forward to real ICD */
        sd->real.DestroySwapchainKHR(sd->real_device, swapchain, pAllocator);
    }
}

/* ── vkGetSwapchainImagesKHR ────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetSwapchainImagesKHR(
    VkDevice        device,
    VkSwapchainKHR  swapchain,
    uint32_t       *pSwapchainImageCount,
    VkImage        *pSwapchainImages)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (!sc || !sc->stereo_active || sc->dxgi_mode) {
        if (!sc || !sc->stereo_active) {
            /* Passthrough */
            VkSwapchainKHR real = sc ? sc->real_swapchain : swapchain;
            return sd->real.GetSwapchainImagesKHR(
                sd->real_device, real, pSwapchainImageCount, pSwapchainImages);
        }
    }

    /* DXGI mode: report our stereo render targets */
    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->image_count;
        return VK_SUCCESS;
    }

    uint32_t copy = (*pSwapchainImageCount < sc->image_count)
                    ? *pSwapchainImageCount : sc->image_count;
    for (uint32_t i = 0; i < copy; i++)
        pSwapchainImages[i] = sc->stereo_images[i];
    *pSwapchainImageCount = copy;
    return (copy < sc->image_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ── vkAcquireNextImageKHR ───────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_AcquireNextImageKHR(
    VkDevice        device,
    VkSwapchainKHR  swapchain,
    uint64_t        timeout,
    VkSemaphore     semaphore,
    VkFence         fence,
    uint32_t       *pImageIndex)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (!sc || !sc->dxgi_mode) {
        VkSwapchainKHR real = sc ? sc->real_swapchain : swapchain;
        return sd->real.AcquireNextImageKHR(
            sd->real_device, real, timeout, semaphore, fence, pImageIndex);
    }

    /* DXGI mode: round-robin image index.
     * The stage_fences[idx] is signaled when the previous staging submit
     * for this slot has completed (so stage_buf is safe to reuse).
     * We wait here to enforce back-pressure.                            */
    uint32_t idx = sc->acquire_idx % sc->image_count;
    sc->acquire_idx++;

    /* Wait for previous staging of this slot (CPU-side throttle) */
    VkResult wres = sd->real.WaitForFences(
        sd->real_device, 1, &sc->stage_fences[idx], VK_TRUE, timeout);
    if (wres != VK_SUCCESS) return wres;  /* VK_TIMEOUT or error */

    /* Signal the app's semaphore/fence via a no-op submit.
     * commandBufferCount=0 is valid in Vulkan 1.1+.              */
    if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
        VkSubmitInfo sig = {
            .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = (semaphore != VK_NULL_HANDLE) ? 1 : 0,
            .pSignalSemaphores    = &semaphore,
        };
        /* If caller supplied a fence, it must not be currently pending.
         * It is safe here because the app follows the acquire→present
         * rule and won't reuse a fence before it's signaled.          */
        VkResult sres = sd->real.QueueSubmit(sd->gfx_queue, 1, &sig, fence);
        if (sres != VK_SUCCESS) return sres;
    }

    *pImageIndex = idx;
    return VK_SUCCESS;
}

/* ── vkQueuePresentKHR ───────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    STEREO_LOG("stereo_QueuePresentKHR: queue=%p swapchainCount=%u",
               (void*)queue, pPresentInfo ? pPresentInfo->swapchainCount : 0);

    /* ── Find the StereoDevice + StereoSwapchain ─────────────────────── */
    extern StereoDevice g_devices[];
    extern uint32_t     g_device_count;

    StereoDevice    *sd = NULL;
    StereoSwapchain *sc = NULL;

    for (uint32_t d = 0; d < g_device_count && !sd; d++) {
        for (uint32_t p = 0; p < pPresentInfo->swapchainCount; p++) {
            StereoSwapchain *found = stereo_swapchain_lookup(
                &g_devices[d], pPresentInfo->pSwapchains[p]);
            if (found) {
                sd = &g_devices[d];
                sc = found;
                break;
            }
        }
    }

    /* ── Passthrough ─────────────────────────────────────────────────── */
    if (!sd || !sc || !sd->stereo.enabled || !sc->dxgi_mode) {
        StereoDevice *fwd = sd ? sd : (g_device_count > 0 ? &g_devices[0] : NULL);
        if (!fwd) return VK_ERROR_DEVICE_LOST;
        return fwd->real.QueuePresentKHR(queue, pPresentInfo);
    }

    /* ── DXGI stereo present for each swapchain ──────────────────────── */
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        StereoSwapchain *sc_i = stereo_swapchain_lookup(
            sd, pPresentInfo->pSwapchains[i]);
        if (!sc_i || !sc_i->dxgi_mode || !sc_i->stereo_active) {
            /* Not our swap chain, or passthrough */
            continue;
        }

        uint32_t img_idx = pPresentInfo->pImageIndices[i];

        /* Semaphores to wait on: app's render-complete sems for first SC */
        uint32_t           wcount = (i == 0) ? pPresentInfo->waitSemaphoreCount : 0;
        const VkSemaphore *wsems  = (i == 0) ? pPresentInfo->pWaitSemaphores    : NULL;

        VkResult pr = stereo_dxgi_present(sd, queue, sc_i, img_idx, wcount, wsems);
        if (pr != VK_SUCCESS) {
            STEREO_ERR("DXGI present failed for swapchain %u: %d", i, pr);
            result = pr;
        }
    }
    return result;
}

/* ============================================================================
 * vkCreateImageView intercept — pass-through (stereo images are single-layer)
 * ============================================================================ */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImageView(
    VkDevice                        device,
    const VkImageViewCreateInfo    *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkImageView                    *pView)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    /* Pass through unchanged — stereo images are now single-layer, no upgrade needed */
    return sd->real.CreateImageView(sd->real_device, pCreateInfo, pAllocator, pView);
}
