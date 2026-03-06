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
    STEREO_LOG("stereo_CreateRenderPass: device=%p subpasses=%u",
               (void*)device, pCreateInfo ? pCreateInfo->subpassCount : 0);
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

    /* ── Determine whether this is a swapchain (stereo) render pass ──────
     * Only render passes that contain an attachment with
     * finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR are swapchain output
     * passes.  Offscreen passes (cubemap faces, shadow maps, G-buffers, …)
     * never have PRESENT_SRC_KHR and must NOT get multiview: their
     * framebuffer attachments are single-layer images, so injecting
     * viewMask=0x3 causes the GPU to write to layer 1 which does not exist
     * → VK_ERROR_DEVICE_LOST on the first frame.
     */
    bool is_swapchain_pass = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        if (pCreateInfo->pAttachments[i].finalLayout
                == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            is_swapchain_pass = true;
            break;
        }
    }

    if (!is_swapchain_pass) {
        /* Passthrough — not the swapchain output pass, do not inject multiview */
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

    /* ── Patch attachment finalLayouts ───────────────────────────────────
     * The app uses finalLayout=PRESENT_SRC_KHR on swapchain attachments.
     * Our stereo images are regular (non-swapchain) images; transitioning
     * them to PRESENT_SRC_KHR is undefined and causes VK_ERROR_DEVICE_LOST.
     * Remap PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL so the image ends
     * in a layout our composite blit can validly transition from.
     */
    VkAttachmentDescription *patched_atts = NULL;
    VkRenderPassCreateInfo modified = *pCreateInfo;
    if (!already_has_mv)
        modified.pNext = &mv_info;

    if (pCreateInfo->attachmentCount > 0) {
        patched_atts = malloc(pCreateInfo->attachmentCount
                              * sizeof(VkAttachmentDescription));
        if (!patched_atts) {
            free(view_masks); free(corr_masks); free(view_offsets);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            patched_atts[i] = pCreateInfo->pAttachments[i];
            if (patched_atts[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                patched_atts[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        modified.pAttachments = patched_atts;
    }

    VkResult res = sd->real.CreateRenderPass(
        sd->real_device, &modified, pAllocator, pRenderPass);

    free(patched_atts);
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
/*
 * VK_KHR_create_renderpass2 is NOT in Vulkan 1.1 core (promoted only in 1.2).
 * If the real driver does not support it (sd->real.CreateRenderPass2KHR == NULL)
 * we downgrade the call to vkCreateRenderPass by converting the
 * VkRenderPassCreateInfo2 subpasses into VkSubpassDescription structs and
 * prepending a VkRenderPassMultiviewCreateInfo.  This preserves stereo on
 * drivers that only support Vulkan 1.1 (e.g. NVIDIA 1.1.117).
 */

/* Convert VkRenderPassCreateInfo2 → VkRenderPassCreateInfo + multiview chain */
#ifdef VK_KHR_create_renderpass2
static VkResult create_rp2_via_rp1(
    StereoDevice                    *sd,
    const VkRenderPassCreateInfo2   *ci2,
    const VkAllocationCallbacks     *pAllocator,
    VkRenderPass                    *pRenderPass)
{
    uint32_t sc = ci2->subpassCount;
    uint32_t ac = ci2->attachmentCount;
    uint32_t dc = ci2->dependencyCount;

    /* Convert attachments */
    VkAttachmentDescription *atts = malloc(ac * sizeof(VkAttachmentDescription));
    if (!atts) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < ac; i++) {
        const VkAttachmentDescription2 *a2 = &ci2->pAttachments[i];
        atts[i] = (VkAttachmentDescription){
            .flags          = a2->flags,
            .format         = a2->format,
            .samples        = a2->samples,
            .loadOp         = a2->loadOp,
            .storeOp        = a2->storeOp,
            .stencilLoadOp  = a2->stencilLoadOp,
            .stencilStoreOp = a2->stencilStoreOp,
            .initialLayout  = a2->initialLayout,
            .finalLayout    = a2->finalLayout,
        };
    }

    /* Convert subpasses */
    VkSubpassDescription *subs = malloc(sc * sizeof(VkSubpassDescription));
    /* We also need storage for the attachment reference arrays */
    VkAttachmentReference **input_refs  = calloc(sc, sizeof(VkAttachmentReference*));
    VkAttachmentReference **color_refs  = calloc(sc, sizeof(VkAttachmentReference*));
    VkAttachmentReference **resolve_refs= calloc(sc, sizeof(VkAttachmentReference*));
    VkAttachmentReference  *depth_refs  = calloc(sc, sizeof(VkAttachmentReference));
    if (!subs || !input_refs || !color_refs || !resolve_refs || !depth_refs) {
        free(atts); free(subs);
        free(input_refs); free(color_refs); free(resolve_refs); free(depth_refs);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

#define CONV_REFS(out, in2, count)     if (count) {         out = malloc((count) * sizeof(VkAttachmentReference));         if (!out) goto oom;         for (uint32_t _j = 0; _j < (count); _j++) {             out[_j].attachment = (in2)[_j].attachment;             out[_j].layout     = (in2)[_j].layout;         }     }

    for (uint32_t i = 0; i < sc; i++) {
        const VkSubpassDescription2 *s2 = &ci2->pSubpasses[i];
        CONV_REFS(input_refs[i],   s2->pInputAttachments,    s2->inputAttachmentCount)
        CONV_REFS(color_refs[i],   s2->pColorAttachments,    s2->colorAttachmentCount)
        CONV_REFS(resolve_refs[i], s2->pResolveAttachments,  s2->colorAttachmentCount)
        if (s2->pDepthStencilAttachment) {
            depth_refs[i].attachment = s2->pDepthStencilAttachment->attachment;
            depth_refs[i].layout     = s2->pDepthStencilAttachment->layout;
        }
        subs[i] = (VkSubpassDescription){
            .flags                   = s2->flags,
            .pipelineBindPoint       = s2->pipelineBindPoint,
            .inputAttachmentCount    = s2->inputAttachmentCount,
            .pInputAttachments       = input_refs[i],
            .colorAttachmentCount    = s2->colorAttachmentCount,
            .pColorAttachments       = color_refs[i],
            .pResolveAttachments     = resolve_refs[i],
            .pDepthStencilAttachment = s2->pDepthStencilAttachment
                                           ? &depth_refs[i] : NULL,
            .preserveAttachmentCount = s2->preserveAttachmentCount,
            .pPreserveAttachments    = s2->pPreserveAttachments,
        };
    }
#undef CONV_REFS

    /* Convert dependencies */
    VkSubpassDependency *deps = NULL;
    if (dc) {
        deps = malloc(dc * sizeof(VkSubpassDependency));
        if (!deps) goto oom;
        for (uint32_t i = 0; i < dc; i++) {
            const VkSubpassDependency2 *d2 = &ci2->pDependencies[i];
            deps[i] = (VkSubpassDependency){
                .srcSubpass      = d2->srcSubpass,
                .dstSubpass      = d2->dstSubpass,
                .srcStageMask    = d2->srcStageMask,
                .dstStageMask    = d2->dstStageMask,
                .srcAccessMask   = d2->srcAccessMask,
                .dstAccessMask   = d2->dstAccessMask,
                .dependencyFlags = d2->dependencyFlags,
            };
        }
    }

    /* viewMasks come from each VkSubpassDescription2.viewMask in the original.
     * Here we override with STEREO_VIEW_MASK (caller already set them). */
    uint32_t *view_masks   = malloc(sc * sizeof(uint32_t));
    uint32_t *corr_masks   = malloc(sc * sizeof(uint32_t));
    if (!view_masks || !corr_masks) { free(view_masks); free(corr_masks); goto oom; }
    for (uint32_t i = 0; i < sc; i++) {
        view_masks[i] = STEREO_VIEW_MASK;
        corr_masks[i] = STEREO_CORRELATION_MASK;
    }

    VkRenderPassMultiviewCreateInfo mv = {
        .sType                = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .pNext                = NULL,
        .subpassCount         = sc,
        .pViewMasks           = view_masks,
        .dependencyCount      = 0,
        .pViewOffsets         = NULL,
        .correlationMaskCount = sc,
        .pCorrelationMasks    = corr_masks,
    };

    VkRenderPassCreateInfo rp1 = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext           = &mv,
        .flags           = ci2->flags,
        .attachmentCount = ac,
        .pAttachments    = atts,
        .subpassCount    = sc,
        .pSubpasses      = subs,
        .dependencyCount = dc,
        .pDependencies   = deps,
    };

    VkResult res = sd->real.CreateRenderPass(sd->real_device, &rp1, pAllocator, pRenderPass);

    free(view_masks); free(corr_masks);
    free(deps);
    for (uint32_t i = 0; i < sc; i++) {
        free(input_refs[i]); free(color_refs[i]); free(resolve_refs[i]);
    }
    free(input_refs); free(color_refs); free(resolve_refs); free(depth_refs);
    free(subs); free(atts);
    return res;

oom:
    free(view_masks); free(corr_masks); free(deps);
    for (uint32_t i = 0; i < sc; i++) {
        if (input_refs)   free(input_refs[i]);
        if (color_refs)   free(color_refs[i]);
        if (resolve_refs) free(resolve_refs[i]);
    }
    free(input_refs); free(color_refs); free(resolve_refs); free(depth_refs);
    free(subs); free(atts);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
#endif /* VK_KHR_create_renderpass2 */


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

    /* If the real driver does not support VK_KHR_create_renderpass2 (common
     * on Vulkan 1.1 drivers) fall back to vkCreateRenderPass with a
     * VkRenderPassMultiviewCreateInfo chain. */
    bool use_rp2 = (sd->real.CreateRenderPass2KHR != NULL);

    if (!sd->stereo.enabled) {
        if (use_rp2)
            return sd->real.CreateRenderPass2KHR(
                sd->real_device, pCreateInfo, pAllocator, pRenderPass);
        /* Passthrough via downgrade: call rp1 without stereo injection */
        return create_rp2_via_rp1(sd, pCreateInfo, pAllocator, pRenderPass);
    }

    /* Only inject multiview for the swapchain output pass (PRESENT_SRC_KHR) */
    {
        bool is_sc = false;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            if (pCreateInfo->pAttachments[i].finalLayout
                    == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                is_sc = true;
                break;
            }
        }
        if (!is_sc) {
            if (use_rp2)
                return sd->real.CreateRenderPass2KHR(
                    sd->real_device, pCreateInfo, pAllocator, pRenderPass);
            return create_rp2_via_rp1(sd, pCreateInfo, pAllocator, pRenderPass);
        }
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

    /* Patch attachment finalLayouts: PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL */
    VkAttachmentDescription2 *patched_atts2 = NULL;
    if (pCreateInfo->attachmentCount > 0) {
        patched_atts2 = malloc(pCreateInfo->attachmentCount
                               * sizeof(VkAttachmentDescription2));
        if (!patched_atts2) { free(subpasses); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            patched_atts2[i] = pCreateInfo->pAttachments[i];
            if (patched_atts2[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                patched_atts2[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    VkRenderPassCreateInfo2 modified = *pCreateInfo;
    modified.pSubpasses   = subpasses;
    if (patched_atts2) modified.pAttachments = patched_atts2;

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

    VkResult res;
    if (use_rp2) {
        res = sd->real.CreateRenderPass2KHR(
            sd->real_device, &modified, pAllocator, pRenderPass);
    } else {
        STEREO_LOG("RenderPass2: no native support, downgrading to RenderPass1+multiview");
        res = create_rp2_via_rp1(sd, &modified, pAllocator, pRenderPass);
    }

    free(patched_atts2);
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
#endif /* VK_KHR_create_renderpass2 */

