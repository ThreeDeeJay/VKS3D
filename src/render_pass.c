/*
 * render_pass.c — Multiview render pass injection
 *
 * VK_KHR_multiview allows a single render pass to broadcast draws to multiple
 * image array layers simultaneously.  We inject VkRenderPassMultiviewCreateInfo
 * with viewMask=0b11 (both eyes rendered per subpass) into every render pass
 * the application creates.
 *
 * Layout of the multiview attachment images:
 *   - Layer 0: Left eye  (view index 0)
 *   - Layer 1: Right eye (view index 1)
 *
 * The SPIR-V-patched vertex shaders read gl_ViewIndex to apply the correct
 * per-eye clip-space offset from our injected stereo UBO.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* viewMask bit 0 = view 0 (left), bit 1 = view 1 (right) */
#define STEREO_VIEW_MASK        0x3u
#define STEREO_CORRELATION_MASK 0x3u  /* both views are rendered together */

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

    if (!sd->stereo.enabled) {
        /* Stereo disabled — passthrough */
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

    /* ── Check if multiview already present ──────────────────────────── */
    const VkBaseInStructure *node = (const VkBaseInStructure*)pCreateInfo->pNext;
    bool already_has_mv = false;
    while (node) {
        if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
            already_has_mv = true;
            break;
        }
        node = node->pNext;
    }

    uint32_t subpass_count = pCreateInfo->subpassCount;

    /* Per-subpass view masks (all subpasses see both eyes) */
    uint32_t *view_masks  = malloc(subpass_count * sizeof(uint32_t));
    /* Correlation masks: both views rendered in same render pass invocation */
    uint32_t *corr_masks  = malloc(subpass_count * sizeof(uint32_t));
    /* View offsets between subpass dependencies: 0 for all (same frame) */
    int32_t  *view_offsets = calloc(pCreateInfo->dependencyCount, sizeof(int32_t));

    if (!view_masks || !corr_masks || !view_offsets) {
        free(view_masks); free(corr_masks); free(view_offsets);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (uint32_t i = 0; i < subpass_count; i++) {
        view_masks[i]  = STEREO_VIEW_MASK;
        corr_masks[i]  = STEREO_CORRELATION_MASK;
    }

    VkRenderPassMultiviewCreateInfo mv_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .pNext                = pCreateInfo->pNext,  /* preserve existing chain */
        .subpassCount         = subpass_count,
        .pViewMasks           = view_masks,
        .dependencyCount      = pCreateInfo->dependencyCount,
        .pViewOffsets         = view_offsets,
        .correlationMaskCount = subpass_count,
        .pCorrelationMasks    = corr_masks,
    };

    VkRenderPassCreateInfo modified = *pCreateInfo;
    if (!already_has_mv)
        modified.pNext = &mv_info;

    VkResult res = sd->real.CreateRenderPass(
        sd->real_device, &modified, pAllocator, pRenderPass);

    free(view_masks);
    free(corr_masks);
    free(view_offsets);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = !already_has_mv;
        rpi->view_mask     = STEREO_VIEW_MASK;
        rpi->subpass_count = subpass_count;
        STEREO_LOG("RenderPass %p: multiview injected (viewMask=0x%x, %u subpasses)",
                   (void*)*pRenderPass, STEREO_VIEW_MASK, subpass_count);
    }

    return res;
}

/* ── vkCreateRenderPass2KHR ─────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRenderPass2KHR(
    VkDevice                         device,
    const VkRenderPassCreateInfo2   *pCreateInfo,
    const VkAllocationCallbacks     *pAllocator,
    VkRenderPass                    *pRenderPass)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd || !sd->real.CreateRenderPass2KHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (!sd->stereo.enabled) {
        return sd->real.CreateRenderPass2KHR(
            sd->real_device, pCreateInfo, pAllocator, pRenderPass);
    }

    uint32_t subpass_count = pCreateInfo->subpassCount;

    /* In VkRenderPassCreateInfo2, viewMask is per-subpass inside
     * VkSubpassDescription2.  We need to modify each subpass. */
    VkSubpassDescription2 *subpasses =
        malloc(subpass_count * sizeof(VkSubpassDescription2));
    if (!subpasses) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < subpass_count; i++) {
        subpasses[i]          = pCreateInfo->pSubpasses[i];
        subpasses[i].viewMask = STEREO_VIEW_MASK;
    }

    VkRenderPassCreateInfo2 modified = *pCreateInfo;
    modified.pSubpasses = subpasses;

    /* Correlation masks via VkRenderPassMultiviewCreateInfo chain is not used
     * in RenderPass2; correlatedViewMasks are in VkSubpassDescription2 directly.
     * We could also set it in pCorrelatedViewMasks of a custom struct, but
     * VkRenderPassCreateInfo2 has its own correlatedViewMaskCount field. */
    uint32_t *corr_masks = malloc(subpass_count * sizeof(uint32_t));
    if (!corr_masks) { free(subpasses); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for (uint32_t i = 0; i < subpass_count; i++)
        corr_masks[i] = STEREO_CORRELATION_MASK;

    modified.correlatedViewMaskCount = subpass_count;
    modified.pCorrelatedViewMasks    = corr_masks;

    VkResult res = sd->real.CreateRenderPass2KHR(
        sd->real_device, &modified, pAllocator, pRenderPass);

    free(subpasses);
    free(corr_masks);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = true;
        rpi->view_mask     = STEREO_VIEW_MASK;
        rpi->subpass_count = subpass_count;
        STEREO_LOG("RenderPass2 %p: multiview set (viewMask=0x%x)", (void*)*pRenderPass, STEREO_VIEW_MASK);
    }

    return res;
}
