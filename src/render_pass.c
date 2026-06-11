/*
 * render_pass.c — Multiview render pass injection (ALL passes)
 *
 * Injects VK_KHR_multiview (viewMask=0x3) into EVERY render pass so the GPU
 * renders both eye layers in a single geometry pass for complete deferred
 * stereo support (G-buffer, shadow, lighting, post-FX, swapchain).
 *
 * Combined with:
 *   stereo_CreateImage     : ALL 2D attachment images upgraded to arrayLayers=2
 *   stereo_CreateImageView : ALL attachment views upgraded to VK_IMAGE_VIEW_TYPE_2D_ARRAY
 *   shader.c geometry path : VS patched with gl_ViewIndex (per-eye geometry)
 *   shader.c quad path     : FS patched with sampler2DArray + gl_ViewIndex
 *                            (deferred/post-fx passes read correct eye layer)
 *
 * PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL layout patch still applied to
 * any attachment that uses it (the final swapchain pass).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

#define STEREO_VIEW_MASK        0x3u
#define STEREO_CORRELATION_MASK 0x3u

/* ── vkCreateRenderPass ─────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRenderPass(
    VkDevice                        device,
    const VkRenderPassCreateInfo   *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkRenderPass                   *pRenderPass)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("stereo_CreateRenderPass: attachments=%u",
               pCreateInfo ? pCreateInfo->attachmentCount : 0);

    /* If stereo disabled or multiview disabled: still patch PRESENT_SRC_KHR
     * layout (required so our plain VkImage render target can accept it),
     * but skip multiview injection. */
    if (!sd->stereo.enabled || !sd->stereo.multiview) {
        /* Check if any attachment needs layout patch */
        bool needs_patch = false;
        for (uint32_t i = 0; pCreateInfo && i < pCreateInfo->attachmentCount; i++) {
            if (pCreateInfo->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                { needs_patch = true; break; }
        }
        if (!needs_patch) {
            VkResult res = sd->real.CreateRenderPass(
                sd->real_device, pCreateInfo, pAllocator, pRenderPass);
            if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
                StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
                rpi->handle = *pRenderPass; rpi->has_multiview = false;
                rpi->view_mask = 0; rpi->subpass_count = pCreateInfo->subpassCount;
            }
            return res;
        }
        VkAttachmentDescription *pa =
            malloc(pCreateInfo->attachmentCount * sizeof(VkAttachmentDescription));
        if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            pa[i] = pCreateInfo->pAttachments[i];
            if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderPassCreateInfo mod = *pCreateInfo;
        mod.pAttachments = pa;
        VkResult res = sd->real.CreateRenderPass(sd->real_device, &mod, pAllocator, pRenderPass);
        free(pa);
        if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
            StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
            rpi->handle = *pRenderPass; rpi->has_multiview = false;
            rpi->view_mask = 0; rpi->subpass_count = pCreateInfo->subpassCount;
        }
        return res;
    }

    /* ── Multiview injection for ALL render passes ─────────────────────────
     * Patch PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL for any attachment
     * that has it (the swapchain output attachment).  All other attachments
     * (G-buffer, shadow map, lighting output) keep their original layouts.  */
    VkAttachmentDescription *patched = NULL;
    if (pCreateInfo->attachmentCount > 0) {
        patched = malloc(pCreateInfo->attachmentCount * sizeof(VkAttachmentDescription));
        if (!patched) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            patched[i] = pCreateInfo->pAttachments[i];
            if (patched[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                patched[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    VkRenderPassCreateInfo modified = *pCreateInfo;
    if (patched) modified.pAttachments = patched;

    uint32_t sc = pCreateInfo->subpassCount;
    uint32_t *view_masks   = malloc(sc * sizeof(uint32_t));
    uint32_t *corr_masks   = malloc(sc * sizeof(uint32_t));
    int32_t  *view_offsets = pCreateInfo->dependencyCount
        ? calloc(pCreateInfo->dependencyCount, sizeof(int32_t)) : NULL;

    if (!view_masks || !corr_masks ||
        (pCreateInfo->dependencyCount && !view_offsets)) {
        free(patched); free(view_masks); free(corr_masks); free(view_offsets);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; i < sc; i++) {
        view_masks[i] = STEREO_VIEW_MASK;
        corr_masks[i] = STEREO_CORRELATION_MASK;
    }
    VkRenderPassMultiviewCreateInfo mv = {
        .sType                = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .pNext                = pCreateInfo->pNext,
        .subpassCount         = sc,
        .pViewMasks           = view_masks,
        .dependencyCount      = pCreateInfo->dependencyCount,
        .pViewOffsets         = view_offsets,
        .correlationMaskCount = sc,
        .pCorrelationMasks    = corr_masks,
    };
    modified.pNext = &mv;

    VkResult res = sd->real.CreateRenderPass(
        sd->real_device, &modified, pAllocator, pRenderPass);

    free(patched); free(view_masks); free(corr_masks); free(view_offsets);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = true;
        rpi->view_mask     = STEREO_VIEW_MASK;
        rpi->subpass_count = sc;
        sd->multiview_pass_exists = true;
        STEREO_LOG("RenderPass %p: multiview injected (viewMask=0x%x, %u att, %u subpasses)",
                   (void*)*pRenderPass, STEREO_VIEW_MASK,
                   pCreateInfo->attachmentCount, sc);
    } else if (res != VK_SUCCESS) {
        STEREO_ERR("RenderPass: multiview injection failed: %d (att=%u)", res,
                   pCreateInfo->attachmentCount);
    }
    return res;
}

/* ── vkCreateRenderPass2KHR ─────────────────────────────────────────────── */
#ifdef VK_KHR_create_renderpass2
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRenderPass2KHR(
    VkDevice                         device,
    const VkRenderPassCreateInfo2   *pCreateInfo,
    const VkAllocationCallbacks     *pAllocator,
    VkRenderPass                    *pRenderPass)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    if (!sd->real.CreateRenderPass2KHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    uint32_t ac = pCreateInfo->attachmentCount;
    uint32_t sc = pCreateInfo->subpassCount;

    /* Patch PRESENT_SRC_KHR layouts */
    VkAttachmentDescription2 *patched_atts = NULL;
    if (ac > 0) {
        patched_atts = malloc(ac * sizeof(VkAttachmentDescription2));
        if (!patched_atts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < ac; i++) {
            patched_atts[i] = pCreateInfo->pAttachments[i];
            if (patched_atts[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                patched_atts[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    VkRenderPassCreateInfo2 modified = *pCreateInfo;
    if (patched_atts) modified.pAttachments = patched_atts;

    if (!sd->stereo.enabled || !sd->stereo.multiview) {
        VkResult res = sd->real.CreateRenderPass2KHR(
            sd->real_device, &modified, pAllocator, pRenderPass);
        free(patched_atts);
        if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
            StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
            rpi->handle = *pRenderPass; rpi->has_multiview = false;
            rpi->view_mask = 0; rpi->subpass_count = sc;
        }
        return res;
    }

    VkSubpassDescription2 *subpasses = malloc(sc * sizeof(VkSubpassDescription2));
    uint32_t *corr = malloc(sc * sizeof(uint32_t));
    if (!subpasses || !corr) {
        free(patched_atts); free(subpasses); free(corr);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; i < sc; i++) {
        subpasses[i]          = pCreateInfo->pSubpasses[i];
        subpasses[i].viewMask = STEREO_VIEW_MASK;
        corr[i]               = STEREO_CORRELATION_MASK;
    }
    modified.pSubpasses              = subpasses;
    modified.correlatedViewMaskCount = sc;
    modified.pCorrelatedViewMasks    = corr;

    VkResult res = sd->real.CreateRenderPass2KHR(
        sd->real_device, &modified, pAllocator, pRenderPass);
    free(patched_atts); free(subpasses); free(corr);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle = *pRenderPass; rpi->has_multiview = true;
        rpi->view_mask = STEREO_VIEW_MASK; rpi->subpass_count = sc;
        sd->multiview_pass_exists = true;
        STEREO_LOG("RenderPass2 %p: multiview injected (viewMask=0x%x, %u att)",
                   (void*)*pRenderPass, STEREO_VIEW_MASK, ac);
    }
    return res;
}
#endif /* VK_KHR_create_renderpass2 */
