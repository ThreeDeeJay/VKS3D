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

    /* CPU staging NOT created here — caller adds it for DX9, not for GPU compose */
    return VK_SUCCESS;
}

/* ── vkCreateSwapchainKHR ──────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateSwapchainKHR(VkDevice device,
                          const VkSwapchainCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks    *pAllocator,
                          VkSwapchainKHR                 *pSwapchain)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    if (!sd->stereo.enabled || sd->swapchain_count >= MAX_SWAPCHAINS)
        return sd->real.CreateSwapchainKHR(sd->real_device, pCreateInfo, pAllocator, pSwapchain);

    uint32_t app_w = pCreateInfo->imageExtent.width;
    uint32_t app_h = pCreateInfo->imageExtent.height;

    StereoSwapchain *sc = &sd->swapchains[sd->swapchain_count];
    memset(sc, 0, sizeof(*sc));
    sc->device     = sd->real_device;
    sc->app_width  = app_w;
    sc->app_height = app_h;
    sc->format     = pCreateInfo->imageFormat;
    sc->hwnd       = stereo_si_hwnd_for_surface(sd->si, pCreateInfo->surface);

    StereoPresentMode req = sd->stereo.present_mode;

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
            sc->app_handle    = (VkSwapchainKHR)(uintptr_t)sc;
            *pSwapchain       = sc->app_handle;
            sd->stereo_w = app_w; sd->stereo_h = app_h;
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
                sc->app_handle    = (VkSwapchainKHR)(uintptr_t)sc;
                *pSwapchain       = sc->app_handle;
                sd->stereo_w = app_w; sd->stereo_h = app_h;
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
        if (sc->hwnd && gpu_compose_sc_init(sd, sc, pCreateInfo->surface)) {
            VkResult res = alloc_alt_stereo_swapchain(sd, sc);
            /* No CPU staging — GPU blit reads directly from stereo_images[0] */
            if (res == VK_SUCCESS && setup_barrier_resources(sd, sc)) {
                sc->present_mode  = req;
                sc->dxgi_mode     = false;
                sc->stereo_active = true;
                /* sc->real_swapchain already set by gpu_compose_sc_init */
                sc->app_handle    = (VkSwapchainKHR)(uintptr_t)sc;
                *pSwapchain       = sc->app_handle;
                sd->stereo_w = app_w; sd->stereo_h = app_h;
                sd->swapchain_count++;
                STEREO_LOG("GPU-blit stereo swapchain (mode=%d): %ux%u  handle=%p",
                           (int)req, app_w, app_h, (void*)*pSwapchain);
                return VK_SUCCESS;
            }
            /* GPU compose init failed — fall to passthrough */
            gpu_compose_sc_destroy(sd, sc);
            if (sc->real_swapchain) {
                sd->real.DestroySwapchainKHR(sd->real_device, sc->real_swapchain, NULL);
                sc->real_swapchain = VK_NULL_HANDLE;
            }
        }
    }

passthrough:
    STEREO_ERR("All stereo modes failed — passthrough");
    sc->stereo_active = false;
    VkResult res = sd->real.CreateSwapchainKHR(sd->real_device, pCreateInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS) {
        sc->real_swapchain = *pSwapchain;
        sc->app_handle     = *pSwapchain;
        sd->swapchain_count++;
    }
    return res;
}

/* ── vkDestroySwapchainKHR ──────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *pAllocator)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (sc) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->stereo_views_arr && sc->stereo_views_arr[i])
                sd->real.DestroyImageView(sd->real_device, sc->stereo_views_arr[i], NULL);
            if (sc->stereo_images && sc->stereo_images[i])
                sd->real.DestroyImage(sd->real_device, sc->stereo_images[i], NULL);
            if (sc->stereo_memory && sc->stereo_memory[i])
                sd->real.FreeMemory(sd->real_device, sc->stereo_memory[i], NULL);
            if (sc->barrier_fences && sc->barrier_fences[i])
                sd->real.DestroyFence(sd->real_device, sc->barrier_fences[i], NULL);
        }
        free(sc->stereo_views_arr); free(sc->stereo_images);
        free(sc->stereo_memory);    free(sc->barrier_cmds);
        free(sc->barrier_fences);

        if (sc->barrier_pool)
            sd->real.DestroyCommandPool(sd->real_device, sc->barrier_pool, NULL);

        gpu_compose_sc_destroy(sd, sc);     /* semaphores + comp_sc_images array */
        alt_cpu_staging_destroy(sd, sc);    /* DX9 CPU staging (no-op if unused) */
        dxgi_sc_destroy(sc);

        /* real_swapchain: GPU compose output SC or passthrough SC */
        if (sc->real_swapchain)
            sd->real.DestroySwapchainKHR(sd->real_device, sc->real_swapchain, pAllocator);

        uint32_t idx = (uint32_t)(sc - sd->swapchains);
        if (idx + 1 < sd->swapchain_count)
            memmove(&sd->swapchains[idx], &sd->swapchains[idx + 1],
                    (sd->swapchain_count - idx - 1) * sizeof(StereoSwapchain));
        sd->swapchain_count--;
    } else {
        sd->real.DestroySwapchainKHR(sd->real_device, swapchain, pAllocator);
    }
}

/* ── vkGetSwapchainImagesKHR ────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                              uint32_t *pCount, VkImage *pImages)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (!sc || !sc->stereo_active) {
        VkSwapchainKHR real = sc ? sc->real_swapchain : swapchain;
        return sd->real.GetSwapchainImagesKHR(sd->real_device, real, pCount, pImages);
    }
    if (!pImages) { *pCount = sc->image_count; return VK_SUCCESS; }
    uint32_t copy = (*pCount < sc->image_count) ? *pCount : sc->image_count;
    for (uint32_t i = 0; i < copy; i++) pImages[i] = sc->stereo_images[i];
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
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("stereo_AcquireNextImageKHR: sc=%p", (void*)swapchain);

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (!sc || !sc->stereo_active) {
        VkSwapchainKHR real = sc ? sc->real_swapchain : swapchain;
        return sd->real.AcquireNextImageKHR(sd->real_device, real,
                                             timeout, semaphore, fence, pImageIndex);
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

    /* Only upgrade images at EXACTLY the swapchain extent.
     * Shadow maps (2048x2048), environment probes (1284x1284), and other
     * auxiliary images at different sizes are left at arrayLayers=1 so
     * their framebuffers remain non-multiview (no per-eye shadow artifacts). */
    bool base = sd->stereo.enabled && sd->stereo.multiview
        && pCreateInfo
        && pCreateInfo->imageType   == VK_IMAGE_TYPE_2D
        && pCreateInfo->arrayLayers == 1
        && pCreateInfo->samples     == VK_SAMPLE_COUNT_1_BIT
        && sd->stereo_w > 0 /* swapchain must be created first */
        && pCreateInfo->extent.width  == sd->stereo_w
        && pCreateInfo->extent.height == sd->stereo_h;

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

    if (!intercept_depth && !intercept_color)
        return sd->real.CreateImage(sd->real_device, pCreateInfo, pAllocator, pImage);

    VkImageCreateInfo modified = *pCreateInfo;
    modified.arrayLayers = 2;
    VkResult res = sd->real.CreateImage(sd->real_device, &modified, pAllocator, pImage);
    if (res == VK_SUCCESS) {
        if (intercept_depth && sd->intercepted_depth_count < MAX_DEPTH_IMAGES)
            sd->intercepted_depth[sd->intercepted_depth_count++] = *pImage;
        if (intercept_color && sd->intercepted_color_count < MAX_COLOR_IMAGES)
            sd->intercepted_color[sd->intercepted_color_count++] = *pImage;
        STEREO_LOG("stereo_CreateImage: upgraded %p → arrayLayers=2 (%s) [%ux%u mip=%u]",
                   (void*)*pImage,
                   intercept_depth ? "depth" : "color",
                   pCreateInfo->extent.width, pCreateInfo->extent.height,
                   pCreateInfo->mipLevels);
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

    bool needs_upgrade = false;
    for (uint32_t si = 0; si < sd->swapchain_count && !needs_upgrade; si++) {
        StereoSwapchain *scc = &sd->swapchains[si];
        if (!scc->stereo_active || !scc->stereo_images) continue;
        for (uint32_t ii = 0; ii < scc->image_count && !needs_upgrade; ii++)
            if (scc->stereo_images[ii] == pCreateInfo->image) needs_upgrade = true;
    }
    for (uint32_t i = 0; i < sd->intercepted_depth_count && !needs_upgrade; i++)
        if (sd->intercepted_depth[i] == pCreateInfo->image) needs_upgrade = true;
    for (uint32_t i = 0; i < sd->intercepted_color_count && !needs_upgrade; i++)
        if (sd->intercepted_color[i] == pCreateInfo->image) needs_upgrade = true;

    if (!needs_upgrade)
        return sd->real.CreateImageView(sd->real_device, pCreateInfo, pAllocator, pView);

    VkImageViewCreateInfo upgraded = *pCreateInfo;
    if (upgraded.viewType == VK_IMAGE_VIEW_TYPE_2D)
        upgraded.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (upgraded.subresourceRange.layerCount < 2)
        upgraded.subresourceRange.layerCount = 2;
    STEREO_LOG("stereo_CreateImageView: upgraded %p → 2D_ARRAY/layerCount=2 [multiview=1]",
               (void*)(uintptr_t)pCreateInfo->image);
    VkResult _r = sd->real.CreateImageView(sd->real_device, &upgraded, pAllocator, pView);
    /* Track upgraded views for framebuffer multiview detection */
    if (_r == VK_SUCCESS && sd->upgraded_view_count < MAX_UPGRADED_VIEWS)
        sd->upgraded_views[sd->upgraded_view_count++] = *pView;
    return _r;
}