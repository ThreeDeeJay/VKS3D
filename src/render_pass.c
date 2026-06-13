/* 
 * render_pass.c — Dual render pass strategy for per-framebuffer multiview
 *
 * stereo_CreateRenderPass creates TWO versions of every render pass:
 *   original (no multiview, but with PRESENT_SRC_KHR patch) — returned to app
 *   mv_handle (multiview viewMask=0x3)                       — stored in rpi
 *
 * stereo_CreateFramebuffer (framebuffer.c) checks whether all attachment
 * image views are "upgraded" (2-layer, created from stereo_CreateImage).
 * If yes → framebuffer is created using mv_handle.
 * If no  → framebuffer uses the original; shadow maps/probes stay non-MV.
 *
 * stereo_CmdBeginRenderPass (framebuffer.c) substitutes the render pass
 * handle in VkRenderPassBeginInfo with mv_handle for MV framebuffers.
 *
 * stereo_CreateGraphicsPipelines (shader.c) substitutes mv_handle so the
 * pipeline is compiled for multiview (gl_ViewIndex receives real view ID).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

#define STEREO_VIEW_MASK        0x3u
#define STEREO_CORRELATION_MASK 0x3u

/* ── Helper: create render pass, patching PRESENT_SRC_KHR → COLOR_ATTACHMENT */
static VkResult create_patched_rp(StereoDevice *sd,
    const VkRenderPassCreateInfo *pCI, const VkAllocationCallbacks *pA,
    VkRenderPass *pRP)
{
    bool needs_patch = false;
    for (uint32_t i = 0; i < pCI->attachmentCount; i++)
        if (pCI->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            { needs_patch = true; break; }
    if (!needs_patch)
        return sd->real.CreateRenderPass(sd->real_device, pCI, pA, pRP);
    VkAttachmentDescription *pa = malloc(pCI->attachmentCount * sizeof(*pa));
    if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < pCI->attachmentCount; i++) {
        pa[i] = pCI->pAttachments[i];
        if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderPassCreateInfo mod = *pCI;  mod.pAttachments = pa;
    VkResult res = sd->real.CreateRenderPass(sd->real_device, &mod, pA, pRP);
    free(pa);  return res;
}

/* ── Helper: create multiview version (PRESENT_SRC_KHR patched + viewMask) */
static VkResult create_mv_rp(StereoDevice *sd,
    const VkRenderPassCreateInfo *pCI, const VkAllocationCallbacks *pA,
    VkRenderPass *pRP)
{
    VkAttachmentDescription *pa = NULL;
    if (pCI->attachmentCount > 0) {
        pa = malloc(pCI->attachmentCount * sizeof(*pa));
        if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < pCI->attachmentCount; i++) {
            pa[i] = pCI->pAttachments[i];
            if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    uint32_t sc = pCI->subpassCount;
    uint32_t *vm = malloc(sc * sizeof(uint32_t));
    uint32_t *cm = malloc(sc * sizeof(uint32_t));
    int32_t  *vo = pCI->dependencyCount ? calloc(pCI->dependencyCount, sizeof(*vo)) : NULL;
    if (!vm || !cm || (pCI->dependencyCount && !vo)) {
        free(pa); free(vm); free(cm); free(vo); return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; i < sc; i++) { vm[i] = STEREO_VIEW_MASK; cm[i] = STEREO_CORRELATION_MASK; }
    VkRenderPassMultiviewCreateInfo mv = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .pNext = pCI->pNext, .subpassCount = sc, .pViewMasks = vm,
        .dependencyCount = pCI->dependencyCount, .pViewOffsets = vo,
        .correlationMaskCount = sc, .pCorrelationMasks = cm,
    };
    VkRenderPassCreateInfo mod = *pCI;
    if (pa) mod.pAttachments = pa;
    mod.pNext = &mv;
    VkResult res = sd->real.CreateRenderPass(sd->real_device, &mod, pA, pRP);
    free(pa); free(vm); free(cm); free(vo);
    return res;
}

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
    STEREO_LOG("stereo_CreateRenderPass: attachments=%u", pCreateInfo->attachmentCount);

    /* Step 1: create the ORIGINAL (non-MV) pass returned to app */
    VkResult res = create_patched_rp(sd, pCreateInfo, pAllocator, pRenderPass);
    if (res != VK_SUCCESS) return res;

    if (sd->render_pass_count >= MAX_RENDER_PASSES) return VK_SUCCESS;
    StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
    rpi->handle        = *pRenderPass;
    rpi->mv_handle     = VK_NULL_HANDLE;
    rpi->has_multiview = false;
    rpi->view_mask     = 0;
    rpi->subpass_count = pCreateInfo->subpassCount;

    /* Step 2: create the multiview version (stored, not returned to app)
     * Only create MV variant when the render pass contains a PRESENT attachment
     * — avoid stereoizing shadow maps / auxiliary passes that do not present. */
    bool has_present = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        if (pCreateInfo->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            has_present = true; break;
        }
    }

    if (sd->stereo.enabled && sd->stereo.multiview && has_present) {
        VkRenderPass mv = VK_NULL_HANDLE;
        if (create_mv_rp(sd, pCreateInfo, pAllocator, &mv) == VK_SUCCESS && mv) {
            rpi->mv_handle     = mv;
            rpi->has_multiview = true;
            rpi->view_mask     = STEREO_VIEW_MASK;
            sd->multiview_pass_exists = true;
            STEREO_LOG("RenderPass %p: original=returned, mv=%p (att=%u, sub=%u)",
                       (void*)*pRenderPass, (void*)mv,
                       pCreateInfo->attachmentCount, pCreateInfo->subpassCount);
        } else {
            STEREO_ERR("RenderPass %p: mv creation failed", (void*)*pRenderPass);
        }
    } else {
        STEREO_LOG("RenderPass %p: mv version skipped (no PRESENT attachment)", (void*)*pRenderPass);
    }
    return VK_SUCCESS;
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
    if (!sd->real.CreateRenderPass2KHR) return VK_ERROR_EXTENSION_NOT_PRESENT;

    uint32_t ac = pCreateInfo->attachmentCount, sc = pCreateInfo->subpassCount;

    /* PRESENT_SRC_KHR patch for original pass */
    VkAttachmentDescription2 *pa = NULL;
    if (ac > 0) {
        pa = malloc(ac * sizeof(*pa));
        if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < ac; i++) {
            pa[i] = pCreateInfo->pAttachments[i];
            if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    VkRenderPassCreateInfo2 orig = *pCreateInfo;
    if (pa) orig.pAttachments = pa;

    VkResult res = sd->real.CreateRenderPass2KHR(sd->real_device, &orig, pAllocator, pRenderPass);
    free(pa);
    if (res != VK_SUCCESS) return res;

    if (sd->render_pass_count >= MAX_RENDER_PASSES) return VK_SUCCESS;
    StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
    rpi->handle = *pRenderPass; rpi->mv_handle = VK_NULL_HANDLE;
    rpi->has_multiview = false; rpi->view_mask = 0; rpi->subpass_count = sc;

    /* Only build multiview version when this pass contains a PRESENT attachment */
    bool has_present2 = false;
    for (uint32_t i = 0; i < ac; i++) {
        if (pCreateInfo->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) { has_present2 = true; break; }
    }

    if (sd->stereo.enabled && sd->stereo.multiview && has_present2) {
        /* Build multiview subpasses */
        VkAttachmentDescription2 *pa2 = NULL;
        if (ac > 0) {
            pa2 = malloc(ac * sizeof(*pa2));
            if (!pa2) return VK_SUCCESS;
            for (uint32_t i = 0; i < ac; i++) {
                pa2[i] = pCreateInfo->pAttachments[i];
                if (pa2[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                    pa2[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        VkSubpassDescription2 *subs = malloc(sc * sizeof(*subs));
        uint32_t *corr = malloc(sc * sizeof(*corr));
        if (subs && corr) {
            for (uint32_t i = 0; i < sc; i++) {
                subs[i] = pCreateInfo->pSubpasses[i];
                subs[i].viewMask = STEREO_VIEW_MASK;
                corr[i] = STEREO_CORRELATION_MASK;
            }
            VkRenderPassCreateInfo2 mv2 = *pCreateInfo;
            if (pa2) mv2.pAttachments = pa2;
            mv2.pSubpasses = subs;
            mv2.correlatedViewMaskCount = sc;
            mv2.pCorrelatedViewMasks    = corr;
            VkRenderPass mv = VK_NULL_HANDLE;
            if (sd->real.CreateRenderPass2KHR(sd->real_device, &mv2, pAllocator, &mv) == VK_SUCCESS && mv) {
                rpi->mv_handle = mv; rpi->has_multiview = true;
                rpi->view_mask = STEREO_VIEW_MASK;
                sd->multiview_pass_exists = true;
                STEREO_LOG("RenderPass2 %p: mv=%p (att=%u)", (void*)*pRenderPass, (void*)mv, ac);
            }
        }
        free(pa2); free(subs); free(corr);
    }
    return VK_SUCCESS;
}
#endif