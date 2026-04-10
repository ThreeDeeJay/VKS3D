/*
 * swapchain.c — DXGI 1.2 stereo swap chain (3D Vision) + passthrough
 *
 * External-memory + multiview architecture
 * ─────────────────────────────────────────
 * The Vulkan render target IS the D3D11 shared texture.  No CPU staging.
 *
 *   1. CreateSwapchainKHR:
 *        dxgi_shared_tex_create  → D3D11 Texture2DArray[2] + NT handle
 *        vkCreateImage(external) + vkAllocateMemory(import NT handle)
 *           → VkImage backed by the same GPU pages as the D3D11 texture
 *      image_count = 1  (single shared image; AcquireNextImage always returns 0)
 *
 *   2. App renders into VkImage via multiview render pass:
 *        layer 0 = left eye,  layer 1 = right eye
 *
 *   3. QueuePresentKHR → present.c:
 *        Submit barrier CB: wait app sems, transition COLOR_ATTACHMENT→GENERAL
 *        WaitForFences
 *        dxgi_copy_and_present: D3D11 CopySubresourceRegion × 2 + Present
 *
 * Depth image interception:
 *   stereo_CreateImage intercepts depth/stencil images whose W×H match the
 *   active DXGI swapchain.  These are forced to arrayLayers=2 so that the
 *   multiview render pass can write the depth pyramid for both eyes without
 *   layer-1 out-of-bounds access.
 *   stereo_CreateImageView upgrades views of intercepted depth images to
 *   VK_IMAGE_VIEW_TYPE_2D_ARRAY / layerCount=2 automatically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"
#include "present_alt.h"

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

/* ── Allocate VkImage backed by imported D3D11 NT-handle memory ─────────
 *
 * Creates a W×H arrayLayers=2 image that shares physical GPU memory with
 * the D3D11 Texture2DArray[2] created by dxgi_shared_tex_create.
 * After this call, sc->shared_nt_handle ownership is consumed by Vulkan.
 * ───────────────────────────────────────────────────────────────────────── */
static VkResult alloc_external_stereo_image(
    StereoDevice    *sd,
    StereoSwapchain *sc,
    VkImage         *out_image,
    VkDeviceMemory  *out_mem)
{
    /* External memory creation info — tells the driver this image will be
     * backed by a D3D11 NT-handle allocation rather than new Vulkan memory */
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
        .arrayLayers   = 2,   /* layer 0 = left eye, layer 1 = right eye */
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = sd->real.CreateImage(sd->real_device, &ici, NULL, out_image);
    if (res != VK_SUCCESS) {
        STEREO_ERR("CreateImage(external) failed: %d", res);
        return res;
    }

    /* Image memory requirements (size, alignment, compatible type bits) */
    VkMemoryRequirements mr;
    sd->real.GetImageMemoryRequirements(sd->real_device, *out_image, &mr);

    /* Query which Vulkan memory types are compatible with the D3D11 handle */
    if (!sd->real.GetMemoryWin32HandlePropertiesKHR) {
        STEREO_ERR("GetMemoryWin32HandlePropertiesKHR not loaded — "
                   "VK_KHR_external_memory_win32 missing?");
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    VkMemoryWin32HandlePropertiesKHR hp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR,
    };
    res = sd->real.GetMemoryWin32HandlePropertiesKHR(
        sd->real_device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        sc->shared_nt_handle,
        &hp);
    if (res != VK_SUCCESS) {
        STEREO_ERR("GetMemoryWin32HandlePropertiesKHR failed: %d", res);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return res;
    }
    STEREO_LOG("Ext mem: image size=%llu  imgTypeBits=0x%x  handleTypeBits=0x%x",
               (unsigned long long)mr.size, mr.memoryTypeBits, hp.memoryTypeBits);

    uint32_t mt = find_memory_type(sd,
        mr.memoryTypeBits & hp.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) {
        STEREO_ERR("No compatible device-local memory type for external image");
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    /* Import: VkDeviceMemory backed by the D3D11 NT handle.
     * Note: NT handles are consumed (ownership transferred) on AllocateMemory. */
    VkImportMemoryWin32HandleInfoKHR import_info = {
        .sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        .handle     = sc->shared_nt_handle,
    };
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &import_info,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, out_mem);
    if (res != VK_SUCCESS) {
        STEREO_ERR("AllocateMemory(import) failed: %d", res);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return res;
    }
    /* Handle ownership now with Vulkan runtime — set to NULL to prevent
     * any stale use (dxgi_sc_destroy already doesn't CloseHandle it) */
    sc->shared_nt_handle = NULL;

    res = sd->real.BindImageMemory(sd->real_device, *out_image, *out_mem, 0);
    if (res != VK_SUCCESS) {
        STEREO_ERR("BindImageMemory(external) failed: %d", res);
        sd->real.FreeMemory(sd->real_device, *out_mem, NULL);
        sd->real.DestroyImage(sd->real_device, *out_image, NULL);
        return res;
    }
    STEREO_LOG("External stereo image: %p  memory=%p  type=%u",
               (void*)*out_image, (void*)*out_mem, mt);
    return VK_SUCCESS;
}

/* ── Shared swapchain setup helpers ────────────────────────────────────── */

/* Allocate the barrier command pool / CB / fence used by the DXGI path. */
static bool setup_barrier_resources(StereoDevice *sd, StereoSwapchain *sc)
{
    VkCommandPoolCreateInfo cpci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = sd->gfx_qf,
    };
    if (sd->real.CreateCommandPool(sd->real_device, &cpci, NULL, &sc->barrier_pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->barrier_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (sd->real.AllocateCommandBuffers(sd->real_device, &cbai, sc->barrier_cmds) != VK_SUCCESS)
        return false;

    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    return sd->real.CreateFence(sd->real_device, &fci, NULL, &sc->barrier_fences[0]) == VK_SUCCESS;
}

/* Allocate a regular (non-external) 2-layer stereo swapchain image + view
 * and CPU staging buffer.  Used by DX9, SBS, TAB, Interlaced modes.     */
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

    /* CPU staging: needed by both DX9 and compose modes */
    return alt_cpu_staging_init(sd, sc);
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

    if (!sd->stereo.enabled || sd->swapchain_count >= MAX_SWAPCHAINS) {
        return sd->real.CreateSwapchainKHR(sd->real_device, pCreateInfo, pAllocator, pSwapchain);
    }

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

    /* ── Try DXGI 1.2 + external memory ───────────────────────────────
     * For AUTO or DXGI mode: attempt the zero-copy external-memory path.  */
    if (req == STEREO_PRESENT_AUTO || req == STEREO_PRESENT_DXGI) {
        bool dxgi_ok = false;
        HANDLE nt_handle = NULL;
        /* Re-entrancy guard: SetFullscreenState inside dxgi_sc_create sends
         * WM_SIZE to the app window. The app calls vkCreateSwapchainKHR again
         * which would call dxgi_sc_create recursively and destroy the swap chain
         * we are currently building. Detect re-entrance and go to passthrough.  */
        if (sd->dxgi_init_in_progress) {
            STEREO_LOG("[DXGI] Re-entrant vkCreateSwapchainKHR during DXGI init — passthrough");
            goto passthrough;
        }
        if (sc->hwnd && dxgi_device_init(sd)) {
            /* Activate NvAPI 3D Vision ONLY for the DXGI stereo path.
             * Must NOT be called for SBS/compose — it crashes on windowed D3D11. */
            dxgi_stereo_activate(sd);
            sd->dxgi_init_in_progress = true;
            if (dxgi_sc_create(sd, sc, &nt_handle)) {
                dxgi_ok = true;
                sd->dxgi_init_in_progress = false;
            } else {
                sd->dxgi_init_in_progress = false;
                STEREO_LOG("[DXGI] path failed — destroying DXGI swap chain and falling back");
                dxgi_sc_destroy(sc);
                STEREO_LOG("[DXGI] DXGI swap chain destroyed, proceeding to DX9/SBS fallback");
            }
        }

        if (dxgi_ok) {
            /* Allocate single-image arrays */
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
            /* Import D3D11 shared memory → VkImage */
            VkResult res = alloc_external_stereo_image(sd, sc,
                &sc->stereo_images[0], &sc->stereo_memory[0]);
            if (res != VK_SUCCESS) {
                STEREO_ERR("External stereo image allocation failed: %d", res);
                dxgi_sc_destroy(sc);
                if (req == STEREO_PRESENT_DXGI) goto passthrough;
                goto try_dx9;
            }
            /* 2D_ARRAY view covering both eye layers */
            VkImageViewCreateInfo vci = {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image    = sc->stereo_images[0],
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format   = sc->format,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 },
            };
            res = sd->real.CreateImageView(sd->real_device, &vci, NULL, &sc->stereo_views_arr[0]);
            if (res != VK_SUCCESS) {
                dxgi_sc_destroy(sc);
                if (req == STEREO_PRESENT_DXGI) goto passthrough;
                goto try_dx9;
            }
            if (!setup_barrier_resources(sd, sc)) {
                dxgi_sc_destroy(sc);
                if (req == STEREO_PRESENT_DXGI) goto passthrough;
                goto try_dx9;
            }
            sc->present_mode  = STEREO_PRESENT_DXGI;
            sc->dxgi_mode     = true;
            sc->stereo_active = true;
            sc->real_swapchain = VK_NULL_HANDLE;
            sc->app_handle    = (VkSwapchainKHR)(uintptr_t)sc;
            *pSwapchain       = sc->app_handle;
            sd->stereo_w = app_w;
            sd->stereo_h = app_h;
            sd->swapchain_count++;
            STEREO_LOG("DXGI stereo swapchain (external mem): %ux%u  handle=%p",
                       app_w, app_h, (void*)*pSwapchain);
            return VK_SUCCESS;
        }

        if (req == STEREO_PRESENT_DXGI) {
            STEREO_ERR("DXGI mode forced but init failed");
            goto passthrough;
        }
        /* AUTO: fall through to DX9 */
    }

try_dx9:
    /* ── DX9 direct mode ──────────────────────────────────────────────
     * Uses NvAPI + IDirect3D9Ex exclusive fullscreen.
     * Requires CPU staging for Vulkan→D3D9 pixel transfer.             */
    if (req == STEREO_PRESENT_AUTO || req == STEREO_PRESENT_DX9) {
        /* Need D3D11/NvAPI already loaded for NvAPI QueryInterface */
        if (!sd->d3d11_ok) dxgi_device_init(sd);
        if (sc->hwnd && dx9_init(sd, sc)) {
            STEREO_LOG("[SBS] alloc_alt_stereo_swapchain...");
            VkResult res = alloc_alt_stereo_swapchain(sd, sc);
            STEREO_LOG("[SBS] alloc_alt_stereo_swapchain result=%d", res);
            if (res == VK_SUCCESS) {
                sc->present_mode  = STEREO_PRESENT_DX9;
                sc->dxgi_mode     = false;
                sc->stereo_active = true;
                sc->real_swapchain = VK_NULL_HANDLE;
                sc->app_handle    = (VkSwapchainKHR)(uintptr_t)sc;
                *pSwapchain       = sc->app_handle;
                sd->stereo_w = app_w;
                sd->stereo_h = app_h;
                sd->swapchain_count++;
                STEREO_LOG("DX9 stereo swapchain: %ux%u  handle=%p",
                           app_w, app_h, (void*)*pSwapchain);
                return VK_SUCCESS;
            }
        }
        if (req == STEREO_PRESENT_DX9) {
            STEREO_ERR("DX9 mode forced but init failed");
            goto passthrough;
        }
        /* AUTO: fall through to SBS */
        STEREO_LOG("DX9 init failed; falling back to SBS compose mode");
        req = STEREO_PRESENT_SBS;
    }

    /* ── Compose modes (SBS / TAB / Interlaced) ──────────────────────── */
    if (req == STEREO_PRESENT_SBS  ||
        req == STEREO_PRESENT_TAB  ||
        req == STEREO_PRESENT_INTERLACED) {
        if (!sd->d3d11_ok) dxgi_device_init(sd);
        STEREO_LOG("[SBS] calling compose_init hwnd=%p d3d11_ok=%d", sc->hwnd, sd->d3d11_ok);
        if (sc->hwnd && compose_init(sd, sc)) {
            STEREO_LOG("[SBS] alloc_alt_stereo_swapchain...");
            VkResult res = alloc_alt_stereo_swapchain(sd, sc);
            STEREO_LOG("[SBS] alloc_alt_stereo_swapchain result=%d", res);
            if (res == VK_SUCCESS) {
                sc->present_mode  = req;
                sc->dxgi_mode     = false;
                sc->stereo_active = true;
                sc->real_swapchain = VK_NULL_HANDLE;
                sc->app_handle    = (VkSwapchainKHR)(uintptr_t)sc;
                *pSwapchain       = sc->app_handle;
                sd->stereo_w = app_w;
                sd->stereo_h = app_h;
                sd->swapchain_count++;
                STEREO_LOG("Compose stereo swapchain (mode=%d): %ux%u  handle=%p",
                           (int)req, app_w, app_h, (void*)*pSwapchain);
                return VK_SUCCESS;
            }
        }
    }

passthrough:
    STEREO_ERR("All stereo modes failed — falling back to passthrough");
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
stereo_DestroySwapchainKHR(
    VkDevice                         device,
    VkSwapchainKHR                   swapchain,
    const VkAllocationCallbacks     *pAllocator)
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
        free(sc->stereo_views_arr);
        free(sc->stereo_images);
        free(sc->stereo_memory);
        free(sc->barrier_cmds);
        free(sc->barrier_fences);

        if (sc->barrier_pool)
            sd->real.DestroyCommandPool(sd->real_device, sc->barrier_pool, NULL);

        /* Destroy CPU staging resources (used by DX9 / compose modes) */
        alt_cpu_staging_destroy(sd, sc);

        dxgi_sc_destroy(sc);

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
stereo_GetSwapchainImagesKHR(
    VkDevice        device,
    VkSwapchainKHR  swapchain,
    uint32_t       *pSwapchainImageCount,
    VkImage        *pSwapchainImages)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
    if (!sc || !sc->stereo_active) {
        VkSwapchainKHR real = sc ? sc->real_swapchain : swapchain;
        return sd->real.GetSwapchainImagesKHR(sd->real_device, real,
                                               pSwapchainImageCount, pSwapchainImages);
    }

    if (!pSwapchainImages) { *pSwapchainImageCount = sc->image_count; return VK_SUCCESS; }

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

    STEREO_LOG("stereo_AcquireNextImageKHR: sc=%p", (void*)swapchain);

    StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);

    /* Passthrough: not a stereo swapchain, or stereo not active */
    if (!sc || !sc->stereo_active) {
        VkSwapchainKHR real = sc ? sc->real_swapchain : swapchain;
        return sd->real.AcquireNextImageKHR(sd->real_device, real,
                                             timeout, semaphore, fence, pImageIndex);
    }

    /* All stereo modes: single-image swapchain, always return index 0.
     * For DXGI mode, wait for the previous present fence (back-pressure).
     * For SBS/DX9/compose modes, no fence to wait on — just signal directly. */
    if (sc->dxgi_mode && sc->barrier_fences && sc->barrier_fences[0]) {
        VkResult wres = sd->real.WaitForFences(
            sd->real_device, 1, &sc->barrier_fences[0], VK_TRUE, timeout);
        if (wres != VK_SUCCESS) return wres;
    }

    /* Signal the app's acquire semaphore/fence via a no-op submit */
    if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
        VkSubmitInfo sig = {
            .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = (semaphore != VK_NULL_HANDLE) ? 1 : 0,
            .pSignalSemaphores    = &semaphore,
        };
        VkQueue q = sd->gfx_queue;
        if (q) {
            VkResult sres = sd->real.QueueSubmit(q, 1, &sig, fence);
            if (sres != VK_SUCCESS) return sres;
        }
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

    /* Poll hotkeys once per present */
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
            pr = compose_present(sd, sc_i, queue, wcount, wsems, sc_i->present_mode);
            break;
        default:
            /* MONO or passthrough — just signal fence so next frame can proceed */
            pr = VK_SUCCESS;
            break;
        }
        if (pr != VK_SUCCESS) {
            STEREO_ERR("Present (mode=%d) failed for swapchain %u: %d",
                       (int)sc_i->present_mode, i, pr);
            result = pr;
        }
    }
    return result;
}

/* ============================================================================
 * stereo_CreateImage — intercept depth images for multiview compatibility
 *
 * Multiview render passes require ALL framebuffer attachments (including
 * depth/stencil) to have arrayLayers >= viewCount (2).  The app creates its
 * depth images as single-layer 2D images.  We intercept depth/stencil image
 * creation whose dimensions match the active DXGI swapchain and silently
 * make them arrayLayers=2.
 *
 * The intercepted VkImage handles are recorded in sd->intercepted_depth[] so
 * that stereo_CreateImageView can upgrade their views to 2D_ARRAY/layerCount=2.
 * ============================================================================ */

static bool is_depth_format(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImage(
    VkDevice                        device,
    const VkImageCreateInfo        *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkImage                        *pImage)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("stereo_CreateImage: fmt=%u %ux%u layers=%u", pCreateInfo ? pCreateInfo->format : 0, pCreateInfo ? pCreateInfo->extent.width : 0, pCreateInfo ? pCreateInfo->extent.height : 0, pCreateInfo ? pCreateInfo->arrayLayers : 0);

    /* Check if this is a depth image we should make 2-layer.
     * Only when multiview=1: matches Sascha Willems reference where both
     * color and depth must be arrayLayers=2 for the multiview framebuffer. */
    bool intercept = sd->stereo.enabled
        && sd->stereo.multiview
        && sd->stereo_w > 0
        && is_depth_format(pCreateInfo->format)
        && pCreateInfo->imageType   == VK_IMAGE_TYPE_2D
        && pCreateInfo->arrayLayers == 1
        && pCreateInfo->extent.width  == sd->stereo_w
        && pCreateInfo->extent.height == sd->stereo_h
        && pCreateInfo->samples == VK_SAMPLE_COUNT_1_BIT;

    if (!intercept) {
        return sd->real.CreateImage(sd->real_device, pCreateInfo, pAllocator, pImage);
    }

    VkImageCreateInfo modified = *pCreateInfo;
    modified.arrayLayers = 2;
    VkResult res = sd->real.CreateImage(sd->real_device, &modified, pAllocator, pImage);
    if (res == VK_SUCCESS && sd->intercepted_depth_count < MAX_DEPTH_IMAGES) {
        sd->intercepted_depth[sd->intercepted_depth_count++] = *pImage;
        STEREO_LOG("CreateImage: patched depth image %p to arrayLayers=2 (%s %ux%u)",
                   (void*)*pImage,
                   modified.extent.width == sd->stereo_w ? "matches swapchain" : "?",
                   modified.extent.width, modified.extent.height);
    }
    return res;
}

/* ============================================================================
 * stereo_CreateImageView — upgrade views for stereo images and patched depth
 *
 * Two classes of images need their views upgraded to 2D_ARRAY/layerCount=2:
 *   1. Stereo color render targets (stereo_images[]) — the app gets these
 *      handles from GetSwapchainImagesKHR and creates plain 2D views.
 *   2. Depth images intercepted by stereo_CreateImage above.
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
    STEREO_LOG("stereo_CreateImageView: image=%p type=%u", pCreateInfo ? (void*)(uintptr_t)pCreateInfo->image : NULL, pCreateInfo ? pCreateInfo->viewType : 0);

    /* When multiview=1 we need 2D_ARRAY views on:
     *   1. Stereo color images (stereo_images[]) — app creates plain 2D views
     *   2. Depth images upgraded by stereo_CreateImage to arrayLayers=2
     * This matches the Sascha Willems multiview example where both color and
     * depth attachments are VK_IMAGE_VIEW_TYPE_2D_ARRAY with layerCount=2.
     * Without this upgrade the multiview framebuffer is incomplete and the GPU
     * hangs (VK_ERROR_DEVICE_LOST) when multiview tries to write to layer 1. */
    if (!sd->stereo.multiview) {
        return sd->real.CreateImageView(sd->real_device, pCreateInfo, pAllocator, pView);
    }

    bool needs_upgrade = false;

    /* Check stereo color images */
    for (uint32_t si = 0; si < sd->swapchain_count && !needs_upgrade; si++) {
        StereoSwapchain *sc = &sd->swapchains[si];
        if (!sc->stereo_active || !sc->stereo_images) continue;
        for (uint32_t ii = 0; ii < sc->image_count && !needs_upgrade; ii++) {
            if (sc->stereo_images[ii] == pCreateInfo->image)
                needs_upgrade = true;
        }
    }

    /* Check patched depth images */
    for (uint32_t i = 0; i < sd->intercepted_depth_count && !needs_upgrade; i++) {
        if (sd->intercepted_depth[i] == pCreateInfo->image)
            needs_upgrade = true;
    }

    if (!needs_upgrade) {
        return sd->real.CreateImageView(sd->real_device, pCreateInfo, pAllocator, pView);
    }

    VkImageViewCreateInfo upgraded = *pCreateInfo;
    if (upgraded.viewType == VK_IMAGE_VIEW_TYPE_2D)
        upgraded.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (upgraded.subresourceRange.layerCount < 2)
        upgraded.subresourceRange.layerCount = 2;
    STEREO_LOG("CreateImageView: upgraded %p to 2D_ARRAY/layerCount=2 [multiview=1]",
               (void*)pCreateInfo->image);
    return sd->real.CreateImageView(sd->real_device, &upgraded, pAllocator, pView);
}
