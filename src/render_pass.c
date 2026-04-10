/*
 * render_pass.c — Multiview render pass injection
 *
 * Injects VK_KHR_multiview (viewMask=0x3) into swapchain output render
 * passes so the GPU renders both eye layers in a single geometry pass.
 *
 * A render pass is identified as a "swapchain output pass" when any
 * attachment has finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
 * Those attachments are simultaneously patched to COLOR_ATTACHMENT_OPTIMAL
 * because our render target is a plain VkImage, not a swapchain image, and
 * transitioning it to PRESENT_SRC_KHR is undefined behaviour.
 *
 * Offscreen passes (shadow maps, cubemaps, G-buffers) never have
 * PRESENT_SRC_KHR and pass through unchanged.
 *
 * REQUIREMENT: All framebuffer attachments must have arrayLayers >= 2.
 * Color attachments: guaranteed by alloc_external_stereo_image (arrayLayers=2).
 * Depth/stencil attachments: guaranteed by stereo_CreateImage intercept,
 *   which upgrades matching depth images to arrayLayers=2 before the app
 *   creates its framebuffers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* viewMask bit 0 = view 0 (left), bit 1 = view 1 (right) */
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
    STEREO_LOG("stereo_CreateRenderPass: attachments=%u", pCreateInfo ? pCreateInfo->attachmentCount : 0);

    /* Detect swapchain output pass */
    bool is_swapchain_pass = false;
    if (sd->stereo.enabled && pCreateInfo->attachmentCount > 0) {
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            if (pCreateInfo->pAttachments[i].finalLayout
                    == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                is_swapchain_pass = true;
                break;
            }
        }
    }

    if (!is_swapchain_pass) {
        VkResult res = sd->real.CreateRenderPass(
            sd->real_device, pCreateInfo, pAllocator, pRenderPass);
        if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
            StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
            rpi->handle        = *pRenderPass;
            rpi->has_multiview = false;
            rpi->view_mask     = 0;
            rpi->subpass_count = pCreateInfo->subpassCount;
        }
        return res;
    }

    /* ── Patch PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL (always required) ──
     * Our stereo render target is a plain VkImage (not a real swapchain image)
     * so it cannot transition to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.              */
    VkAttachmentDescription *patched =
        malloc(pCreateInfo->attachmentCount * sizeof(VkAttachmentDescription));
    if (!patched) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        patched[i] = pCreateInfo->pAttachments[i];
        if (patched[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            patched[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    /* ── Multiview injection (opt-in via multiview=1 in vks3d.ini) ────────────
     * VK_KHR_multiview renders both eye layers in one geometry pass.
     * REQUIREMENT: ALL framebuffer attachments (including depth) must be
     * VK_IMAGE_VIEW_TYPE_2D_ARRAY with layerCount >= 2.  Most apps use
     * single-layer depth buffers → GPU hang → VK_ERROR_DEVICE_LOST.
     * Only enable for apps that explicitly provide array depth buffers.
     * Default: off.  Enable: set multiview=1 in [global] of vks3d.ini.       */
    VkRenderPassCreateInfo modified = *pCreateInfo;
    modified.pAttachments = patched;

    uint32_t *view_masks = NULL, *corr_masks = NULL;
    int32_t  *view_offsets = NULL;
    VkRenderPassMultiviewCreateInfo mv;

    if (sd->stereo.multiview) {
        uint32_t sc = pCreateInfo->subpassCount;
        view_masks  = malloc(sc * sizeof(uint32_t));
        corr_masks  = malloc(sc * sizeof(uint32_t));
        view_offsets = pCreateInfo->dependencyCount
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
        mv = (VkRenderPassMultiviewCreateInfo){
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
    }

    VkResult res = sd->real.CreateRenderPass(
        sd->real_device, &modified, pAllocator, pRenderPass);

    free(patched); free(view_masks); free(corr_masks); free(view_offsets);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = sd->stereo.multiview;
        rpi->view_mask     = sd->stereo.multiview ? STEREO_VIEW_MASK : 0;
        rpi->subpass_count = pCreateInfo->subpassCount;
        if (sd->stereo.multiview)
            STEREO_LOG("RenderPass %p: multiview injected (viewMask=0x%x, %u subpasses) "
                       "[INI: multiview=1]",
                       (void*)*pRenderPass, STEREO_VIEW_MASK, pCreateInfo->subpassCount);
        else
            STEREO_LOG("RenderPass %p: finalLayout patched (multiview disabled — "
                       "set multiview=1 in vks3d.ini to enable)",
                       (void*)*pRenderPass);
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

    /* Detect swapchain output pass */
    bool is_swapchain_pass = false;
    if (sd->stereo.enabled && pCreateInfo->attachmentCount > 0) {
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            if (pCreateInfo->pAttachments[i].finalLayout
                    == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                is_swapchain_pass = true;
                break;
            }
        }
    }

    if (!is_swapchain_pass) {
        return sd->real.CreateRenderPass2KHR(
            sd->real_device, pCreateInfo, pAllocator, pRenderPass);
    }

    /* Patch attachments + set per-subpass viewMasks */
    uint32_t ac = pCreateInfo->attachmentCount;
    uint32_t sc = pCreateInfo->subpassCount;

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

    /* No multiview injection — see CreateRenderPass comment for rationale */
    VkRenderPassCreateInfo2 modified = *pCreateInfo;
    if (patched_atts) modified.pAttachments = patched_atts;

    if (sd->stereo.multiview) {
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
            rpi->handle        = *pRenderPass;
            rpi->has_multiview = true;
            rpi->view_mask     = STEREO_VIEW_MASK;
            rpi->subpass_count = sc;
            STEREO_LOG("RenderPass2 %p: multiview injected (viewMask=0x%x) [INI: multiview=1]",
                       (void*)*pRenderPass, STEREO_VIEW_MASK);
        }
        return res;
    }

    VkResult res = sd->real.CreateRenderPass2KHR(
        sd->real_device, &modified, pAllocator, pRenderPass);

    free(patched_atts);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = false;
        rpi->view_mask     = 0;
        rpi->subpass_count = pCreateInfo->subpassCount;
        STEREO_LOG("RenderPass2 %p: finalLayout patched (multiview disabled)",
                   (void*)*pRenderPass);
    }
    return res;
}
#endif /* VK_KHR_create_renderpass2 */
