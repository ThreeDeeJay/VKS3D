/*
 * swapchain.c — Side-By-Side swapchain management
 *
 * Strategy
 * ────────
 * The application requests a swapchain of width W × height H.
 * We create a real swapchain of width (2×W) × H.
 *
 * The left half  [0..W)   receives the left eye  (multiview layer 0)
 * The right half [W..2W)  receives the right eye (multiview layer 1)
 *
 * Per-frame resources we allocate alongside the real swapchain:
 *   - A 2-layer image array (W × H, arrayLayers=2) per swapchain image,
 *     used as the multiview render target.
 *   - An image view of the full array (for use as a framebuffer attachment)
 *   - Individual layer views (for the composite blit shader)
 *   - A command pool + command buffers for the composite pass
 *   - Composite render pass, pipeline, framebuffers, descriptor sets
 *
 * At vkAcquireNextImageKHR we forward to the real (2×W) swapchain.
 * At vkQueuePresentKHR we run the composite blit BEFORE presenting.
 *
 * vkGetSwapchainImagesKHR reports the 2-layer images to the app, NOT the
 * real (2×W) swapchain images.  The app renders into the 2-layer images,
 * the composite pass blits them SBS into the real swapchain image.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* GLSL source for the SBS composite pass (compiled inline to SPIR-V as hex) */
/* We embed precompiled SPIR-V for a full-screen-triangle blit shader.
 * Left half samples from layer 0, right half samples from layer 1. */

/* ── Composite shader SPIR-V (pre-compiled) ─────────────────────────────── */
/*
 * Vertex shader (full-screen triangle):
 *   void main() {
 *     vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
 *     gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
 *     outUV = uv;
 *   }
 */

/* Fragment shader:
 *   layout(set=0, binding=0) uniform sampler2DArray tex;
 *   void main() {
 *     float layer = (inUV.x < 0.5) ? 0.0 : 1.0;
 *     float u     = (inUV.x < 0.5) ? inUV.x * 2.0 : (inUV.x - 0.5) * 2.0;
 *     outColor = texture(tex, vec3(u, inUV.y, layer));
 *   }
 */

/* Precompiled SPIR-V words for the composite vertex shader */
static const uint32_t COMPOSITE_VERT_SPV[] = {
    /* SPIR-V header */
    0x07230203, 0x00010300, 0x00070000, 0x00000018, 0x00000000,
    /* Capabilities */
    0x00020011, 0x00000001,  /* OpCapability Shader */
    /* Memory model */
    0x0003000E, 0x00000000, 0x00000001,  /* OpMemoryModel Logical GLSL450 */
    /* Entry point: Vertex, main, gl_Position, gl_VertexIndex, outUV */
    0x000A000F, 0x00000000, 0x00000001, 0x6E69616D, 0x00000000,
                0x00000002, 0x00000003, 0x00000004, 0x00000000, 0x00000000,
    /* Decorations */
    0x00040047, 0x00000002, 0x0000000B, 0x00000000,  /* Position BuiltIn 0    */
    0x00040047, 0x00000003, 0x0000000B, 0x00000002,  /* VertexIndex BuiltIn 42*/
    0x00040047, 0x00000004, 0x0000001E, 0x00000000,  /* outUV Location 0      */
    /* Types */
    0x00020013, 0x00000005,  /* TypeVoid */
    0x00030021, 0x00000006, 0x00000005, /* TypeFunction void */
    0x00030016, 0x00000007, 0x00000020, /* TypeFloat 32 */
    0x00040017, 0x00000008, 0x00000007, 0x00000004, /* TypeVector float 4 */
    0x00040017, 0x00000009, 0x00000007, 0x00000002, /* TypeVector float 2 */
    0x00040020, 0x0000000A, 0x00000003, 0x00000008, /* TypePointer Output v4 */
    0x00040020, 0x0000000B, 0x00000003, 0x00000009, /* TypePointer Output v2 */
    0x00040015, 0x0000000C, 0x00000020, 0x00000000, /* TypeInt 32 0 */
    0x00040020, 0x0000000D, 0x00000001, 0x0000000C, /* TypePointer Input int */
    /* Variables */
    0x0004003B, 0x0000000A, 0x00000002, 0x00000003, /* Variable gl_Position Output */
    0x0004003B, 0x0000000D, 0x00000003, 0x00000001, /* Variable gl_VertexIndex Input */
    0x0004003B, 0x0000000B, 0x00000004, 0x00000003, /* Variable outUV Output */
    /* Function main */
    0x00050036, 0x00000005, 0x00000001, 0x00000000, 0x00000006, /* Function void main */
    0x00020039, 0x00000010,              /* Label */
    /* gl_Position = vec4(0,0,0,1) — simplified; full FST needs bit ops */
    0x00000043, /* (placeholder — real SPIR-V would compute FST position) */
    0x000100FD,  /* OpReturn */
    0x00010038,  /* OpFunctionEnd */
};
/* NOTE: The above is a structural placeholder.  In production, use
 * glslangValidator or shaderc to compile the GLSL and embed the bytes.
 * See shaders/ directory for GLSL sources. */

/* ── Helper: find suitable memory type ──────────────────────────────────── */
static uint32_t find_memory_type(
    StereoDevice *sd,
    uint32_t     type_bits,
    VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(
        sd->real_physdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/* ── Allocate per-frame multiview render target ──────────────────────────── */
static VkResult alloc_stereo_image(
    StereoDevice *sd,
    uint32_t      width, uint32_t height,
    VkFormat      format,
    VkImage      *out_image,
    VkDeviceMemory *out_mem)
{
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 2,           /* one layer per eye */
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
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
    if (mt == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mt,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, out_mem);
    if (res != VK_SUCCESS) return res;

    return sd->real.BindImageMemory(sd->real_device, *out_image, *out_mem, 0);
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
        return sd->real.CreateSwapchainKHR(
            sd->real_device, pCreateInfo, pAllocator, pSwapchain);
    }

    /* ── Double the width for SBS ────────────────────────────────────── */
    VkSwapchainCreateInfoKHR sci = *pCreateInfo;
    uint32_t app_w = sci.imageExtent.width;
    uint32_t app_h = sci.imageExtent.height;
    sci.imageExtent.width = app_w * 2;  /* SBS requires 2× width */

    /* The composite blit writes into the real SBS swapchain images via
     * CmdBlitImage (dst).  The app typically only requests
     * VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; without TRANSFER_DST_BIT the
     * blit is illegal and causes VK_ERROR_DEVICE_LOST on the second submit
     * (the first submit appears to succeed but the GPU state is corrupted).
     * Similarly the stereo render target is a blit source so it needs
     * TRANSFER_SRC_BIT — that is already set in alloc_stereo_image. */
    sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    STEREO_LOG("SBS swapchain: app %ux%u → real %ux%u",
               app_w, app_h, sci.imageExtent.width, app_h);

    VkResult res = sd->real.CreateSwapchainKHR(
        sd->real_device, &sci, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) return res;

    /* ── Register swapchain state ──────────────────────────────────── */
    StereoSwapchain *sc = &sd->swapchains[sd->swapchain_count++];
    memset(sc, 0, sizeof(*sc));
    sc->real_swapchain = *pSwapchain;
    sc->device         = sd->real_device;
    sc->app_width      = app_w;
    sc->app_height     = app_h;
    sc->sbs_width      = app_w * 2;
    sc->format         = pCreateInfo->imageFormat;

    /* Get real swapchain images */
    res = sd->real.GetSwapchainImagesKHR(
        sd->real_device, sc->real_swapchain, &sc->image_count, NULL);
    if (res != VK_SUCCESS) return res;

    sc->sbs_images      = malloc(sc->image_count * sizeof(VkImage));
    sc->stereo_images   = malloc(sc->image_count * sizeof(VkImage));
    sc->stereo_memory   = malloc(sc->image_count * sizeof(VkDeviceMemory));
    sc->stereo_views_l  = malloc(sc->image_count * sizeof(VkImageView));
    sc->stereo_views_r  = malloc(sc->image_count * sizeof(VkImageView));
    sc->stereo_views_arr= malloc(sc->image_count * sizeof(VkImageView));
    sc->composite_framebuffers = malloc(sc->image_count * sizeof(VkFramebuffer));
    sc->composite_cmds  = malloc(sc->image_count * sizeof(VkCommandBuffer));
    sc->composite_desc_sets = malloc(sc->image_count * sizeof(VkDescriptorSet));

    if (!sc->sbs_images || !sc->stereo_images || !sc->stereo_memory ||
        !sc->stereo_views_l || !sc->stereo_views_r || !sc->stereo_views_arr ||
        !sc->composite_framebuffers || !sc->composite_cmds || !sc->composite_desc_sets)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    res = sd->real.GetSwapchainImagesKHR(
        sd->real_device, sc->real_swapchain, &sc->image_count, sc->sbs_images);
    if (res != VK_SUCCESS) return res;

    /* Allocate per-image stereo render targets */
    for (uint32_t i = 0; i < sc->image_count; i++) {
        res = alloc_stereo_image(sd, app_w, app_h, sc->format,
                                  &sc->stereo_images[i], &sc->stereo_memory[i]);
        if (res != VK_SUCCESS) {
            STEREO_ERR("Failed to allocate stereo image %u: %d -- "
                       "falling back to passthrough (no stereo for this swapchain)", i, res);
            /* Zero out partial allocation so GetSwapchainImagesKHR falls back
             * to returning the real SBS images rather than null handles, which
             * would cause an access violation in the app. */
            sc->stereo_active = false;
            for (uint32_t j = 0; j < i; j++) {
                if (sc->stereo_images[j])
                    sd->real.DestroyImage(sd->real_device, sc->stereo_images[j], NULL);
                if (sc->stereo_memory[j])
                    sd->real.FreeMemory(sd->real_device, sc->stereo_memory[j], NULL);
                sc->stereo_images[j] = VK_NULL_HANDLE;
                sc->stereo_memory[j] = VK_NULL_HANDLE;
            }
            /* Swapchain still usable in passthrough mode */
            return VK_SUCCESS;
        }

        /* Array view (for use as framebuffer attachment in multiview render pass) */
        VkImageViewCreateInfo vci_arr = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = sc->stereo_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format   = sc->format,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 2,
            },
        };
        res = sd->real.CreateImageView(sd->real_device, &vci_arr, NULL,
                                        &sc->stereo_views_arr[i]);
        if (res != VK_SUCCESS) return res;

        /* Left eye layer view */
        VkImageViewCreateInfo vci_l = vci_arr;
        vci_l.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci_l.subresourceRange.baseArrayLayer = 0;
        vci_l.subresourceRange.layerCount     = 1;
        res = sd->real.CreateImageView(sd->real_device, &vci_l, NULL,
                                        &sc->stereo_views_l[i]);
        if (res != VK_SUCCESS) return res;

        /* Right eye layer view */
        VkImageViewCreateInfo vci_r = vci_l;
        vci_r.subresourceRange.baseArrayLayer = 1;
        res = sd->real.CreateImageView(sd->real_device, &vci_r, NULL,
                                        &sc->stereo_views_r[i]);
        if (res != VK_SUCCESS) return res;
    }

    sc->stereo_active = true;
    STEREO_LOG("Swapchain %p: %u images, stereo targets allocated",
               (void*)*pSwapchain, sc->image_count);
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
            if (sc->stereo_views_l && sc->stereo_views_l[i])
                sd->real.DestroyImageView(sd->real_device, sc->stereo_views_l[i], NULL);
            if (sc->stereo_views_r && sc->stereo_views_r[i])
                sd->real.DestroyImageView(sd->real_device, sc->stereo_views_r[i], NULL);
            if (sc->stereo_images && sc->stereo_images[i])
                sd->real.DestroyImage(sd->real_device, sc->stereo_images[i], NULL);
            if (sc->stereo_memory && sc->stereo_memory[i])
                sd->real.FreeMemory(sd->real_device, sc->stereo_memory[i], NULL);
        }
        free(sc->sbs_images);
        free(sc->stereo_images);
        free(sc->stereo_memory);
        free(sc->stereo_views_l);
        free(sc->stereo_views_r);
        free(sc->stereo_views_arr);
        free(sc->composite_framebuffers);
        free(sc->composite_cmds);
        free(sc->composite_desc_sets);

        /* Destroy composite resources */
        if (sc->composite_cmd_pool)
            sd->real.DestroyCommandPool(sd->real_device, sc->composite_cmd_pool, NULL);
        if (sc->composite_sampler)
            sd->real.DestroySampler(sd->real_device, sc->composite_sampler, NULL);
        if (sc->composite_pipeline)
            sd->real.DestroyPipeline(sd->real_device, sc->composite_pipeline, NULL);
        if (sc->composite_layout)
            sd->real.DestroyPipelineLayout(sd->real_device, sc->composite_layout, NULL);
        if (sc->composite_dsl)
            sd->real.DestroyDescriptorSetLayout(sd->real_device, sc->composite_dsl, NULL);
        if (sc->composite_pool)
            sd->real.DestroyDescriptorPool(sd->real_device, sc->composite_pool, NULL);
        if (sc->composite_renderpass)
            sd->real.DestroyRenderPass(sd->real_device, sc->composite_renderpass, NULL);

        /* Remove from list */
        stereo_mutex_lock(&sd->lock);
        for (uint32_t i = 0; i < sd->swapchain_count; i++) {
            if (sd->swapchains[i].real_swapchain == swapchain) {
                sd->swapchains[i] = sd->swapchains[--sd->swapchain_count];
                break;
            }
        }
        stereo_mutex_unlock(&sd->lock);
    }

    sd->real.DestroySwapchainKHR(sd->real_device, swapchain, pAllocator);
}

/* ── vkGetSwapchainImagesKHR ─────────────────────────────────────────────── */
/*
 * Report our stereo render target images (not the real SBS swapchain images)
 * to the application.  The app will create framebuffers and render into these.
 * We then blit them SBS into the real swapchain image at present time.
 */
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
    if (!sc) {
        return sd->real.GetSwapchainImagesKHR(
            sd->real_device, swapchain, pSwapchainImageCount, pSwapchainImages);
    }

    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->image_count;
        return VK_SUCCESS;
    }

    uint32_t copy = (*pSwapchainImageCount < sc->image_count)
                  ? *pSwapchainImageCount : sc->image_count;
    for (uint32_t i = 0; i < copy; i++)
        /* Return stereo render target if active, else real SBS image */
        pSwapchainImages[i] = sc->stereo_active ? sc->stereo_images[i] : sc->sbs_images[i];
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
    if (!sc) {
        return sd->real.AcquireNextImageKHR(
            sd->real_device, swapchain, timeout, semaphore, fence, pImageIndex);
    }

    /* Forward to real (doubled-width) swapchain */
    return sd->real.AcquireNextImageKHR(
        sd->real_device, sc->real_swapchain, timeout, semaphore, fence, pImageIndex);
}

/* ── vkQueuePresentKHR ───────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    STEREO_LOG("stereo_QueuePresentKHR: queue=%p swapchainCount=%u",
               (void*)queue, pPresentInfo ? pPresentInfo->swapchainCount : 0);
    /* Find the device for this queue (scan all devices) */
    StereoDevice *sd = NULL;
    StereoSwapchain *sc = NULL;

    /* Walk all devices to find the one owning these swapchains */
    /* (simplified: use global registry from stereo.c) */
    extern StereoDevice g_devices[];
    extern uint32_t     g_device_count;
    for (uint32_t d = 0; d < g_device_count && !sd; d++) {
        for (uint32_t s = 0; s < g_devices[d].swapchain_count; s++) {
            /* Check if any of the present swapchains matches */
            for (uint32_t p = 0; p < pPresentInfo->swapchainCount; p++) {
                if (g_devices[d].swapchains[s].real_swapchain ==
                    pPresentInfo->pSwapchains[p]) {
                    sd = &g_devices[d];
                    sc = &g_devices[d].swapchains[s];
                    break;
                }
            }
        }
    }

    if (!sd || !sc || !sd->stereo.enabled) {
        /* Passthrough or stereo disabled */
        StereoDevice *fwd = sd ? sd : (g_device_count > 0 ? &g_devices[0] : NULL);
        if (!fwd) return VK_ERROR_DEVICE_LOST;
        return fwd->real.QueuePresentKHR(queue, pPresentInfo);
    }

    /* Run the SBS composite pass for each swapchain image being presented.
     *
     * Semaphore handoff:
     *   - pPresentInfo->pWaitSemaphores = app's render-complete semaphores
     *     These are passed to the composite submit so it waits for rendering.
     *   - composite signals done_sems[i] when blit finishes
     *   - done_sems[i] replaces pWaitSemaphores in the real QueuePresentKHR
     *     so the display engine only scans out after composite is done.
     */
    uint32_t n = pPresentInfo->swapchainCount;
    VkSemaphore *done_sems = calloc(n, sizeof(VkSemaphore));
    if (!done_sems) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t img_idx = pPresentInfo->pImageIndices[i];
        /* Only pass app wait semaphores to the first swapchain's composite;
         * subsequent composites within the same present have no additional waits. */
        uint32_t        wcount = (i == 0) ? pPresentInfo->waitSemaphoreCount : 0;
        const VkSemaphore *wsems = (i == 0) ? pPresentInfo->pWaitSemaphores : NULL;
        VkResult composite_res = stereo_composite_to_sbs(
            sd, queue, sc, img_idx, wcount, wsems, &done_sems[i]);
        if (composite_res != VK_SUCCESS) {
            STEREO_ERR("Composite pass failed for image %u: %d", img_idx, composite_res);
            /* Fall through — present anyway, output may be wrong but avoid hang */
        }
    }

    /* Build modified present info: real swapchain handles + composite semaphores */
    VkPresentInfoKHR pi = *pPresentInfo;
    VkSwapchainKHR  *real_scs = malloc(n * sizeof(VkSwapchainKHR));
    if (!real_scs) { free(done_sems); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for (uint32_t i = 0; i < n; i++) {
        StereoSwapchain *sc_i = stereo_swapchain_lookup(sd, pi.pSwapchains[i]);
        real_scs[i] = sc_i ? sc_i->real_swapchain : pi.pSwapchains[i];
    }
    pi.pSwapchains = real_scs;

    /* Replace wait semaphores with composite completion semaphores */
    /* Filter out any NULL (composite failed) entries */
    uint32_t valid = 0;
    for (uint32_t i = 0; i < n; i++)
        if (done_sems[i] != VK_NULL_HANDLE) done_sems[valid++] = done_sems[i];
    if (valid > 0) {
        pi.waitSemaphoreCount = valid;
        pi.pWaitSemaphores    = done_sems;
    }

    VkResult res = sd->real.QueuePresentKHR(queue, &pi);
    free(real_scs);

    /* Destroy composite completion semaphores after present */
    for (uint32_t i = 0; i < valid; i++)
        sd->real.DestroySemaphore(sd->real_device, done_sems[i], NULL);
    free(done_sems);
    return res;
}

/* ============================================================================
 * vkCreateImageView intercept (see image_view.c header for full rationale)
 * ============================================================================ */
/* ---- helper: is this VkImage one of our stereo render targets? --------- */
/*
 * Returns the swapchain that owns 'image', or NULL if it is not a stereo
 * render target.  Caller holds no lock (read-only scan of immutable arrays).
 */
static StereoSwapchain *find_owning_swapchain(StereoDevice *sd, VkImage image)
{
    for (uint32_t si = 0; si < sd->swapchain_count; si++) {
        StereoSwapchain *sc = &sd->swapchains[si];
        if (!sc->stereo_active || !sc->stereo_images) continue;
        for (uint32_t ii = 0; ii < sc->image_count; ii++) {
            if (sc->stereo_images[ii] == image)
                return sc;
        }
    }
    return NULL;
}

/* ---- vkCreateImageView ------------------------------------------------- */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImageView(
    VkDevice                        device,
    const VkImageViewCreateInfo    *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkImageView                    *pView)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* Passthrough for non-stereo devices or disabled stereo */
    if (!sd->stereo.enabled) {
        return sd->real.CreateImageView(sd->real_device, pCreateInfo,
                                        pAllocator, pView);
    }

    /* Is this image one of our stereo render targets? */
    StereoSwapchain *sc = find_owning_swapchain(sd, pCreateInfo->image);

    if (!sc) {
        /* Not a stereo image -- pass through unchanged */
        return sd->real.CreateImageView(sd->real_device, pCreateInfo,
                                        pAllocator, pView);
    }

    /*
     * This is a stereo render target (2-layer array image).
     * The app is creating a view for its framebuffer attachment.
     * Upgrade to VK_IMAGE_VIEW_TYPE_2D_ARRAY covering both layers so
     * the multiview render pass (viewMask=0x3) can write to both eyes.
     *
     * We accept whatever the app requested for baseArrayLayer (usually 0)
     * and simply set layerCount=2 and viewType=2D_ARRAY.
     */
    VkImageViewCreateInfo upgraded = *pCreateInfo;

    if (upgraded.viewType == VK_IMAGE_VIEW_TYPE_2D) {
        upgraded.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        STEREO_LOG("CreateImageView: upgrading stereo image %p view: "
                   "2D/layerCount=%u -> 2D_ARRAY/layerCount=2",
                   (void*)pCreateInfo->image,
                   pCreateInfo->subresourceRange.layerCount);
    }

    /* Always ensure layerCount covers both eyes */
    if (upgraded.subresourceRange.layerCount < 2)
        upgraded.subresourceRange.layerCount = 2;

    return sd->real.CreateImageView(sd->real_device, &upgraded,
                                    pAllocator, pView);
}
