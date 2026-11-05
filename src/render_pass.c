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

static bool is_depth_only_renderpass(
    const VkRenderPassCreateInfo *pCI)
{
    if (!pCI || pCI->attachmentCount != 1)
        return false;

    VkFormat fmt = pCI->pAttachments[0].format;

    switch (fmt) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

static bool is_shadow_renderpass(
    const VkRenderPassCreateInfo *pCI)
{
    if (!pCI || pCI->attachmentCount != 2)
        return false;
    const VkAttachmentDescription *color = &pCI->pAttachments[0];
    const VkAttachmentDescription *depth = &pCI->pAttachments[1];
    if (color->format != VK_FORMAT_R32_SFLOAT)
        return false;
    switch (depth->format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

static bool find_shading_rate_attachment2(
    const VkRenderPassCreateInfo2 *pCI,
    uint32_t *attachment)
{
    if (!pCI || !attachment)
        return false;
    *attachment = VK_ATTACHMENT_UNUSED;
#ifdef VK_KHR_fragment_shading_rate
    for (uint32_t i = 0; i < pCI->subpassCount; i++) {
        const VkBaseInStructure *pNext =
            (const VkBaseInStructure *)pCI->pSubpasses[i].pNext;
        while (pNext) {
            if (pNext->sType ==
                VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) {
                const VkFragmentShadingRateAttachmentInfoKHR *fsr =
                    (const VkFragmentShadingRateAttachmentInfoKHR *)pNext;
                if (fsr->pFragmentShadingRateAttachment) {
                    *attachment =
                        fsr->pFragmentShadingRateAttachment->attachment;
                    return *attachment != VK_ATTACHMENT_UNUSED;
                }
            }
            pNext = pNext->pNext;
        }
    }
#endif
    return false;
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
    for (uint32_t i = 0; i < sc; i++) { vm[i] = 0x00000003; cm[i] = STEREO_CORRELATION_MASK; }
    STEREO_LOG(
        "MV_RP_CREATE subpasses=%u viewMask0=0x%x correlationMask0=0x%x",
        sc,
        sc ? vm[0] : 0,
        sc ? cm[0] : 0);
    VkRenderPassMultiviewCreateInfo mv = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
        .pNext = NULL, .subpassCount = sc, .pViewMasks = vm,
        .dependencyCount = pCI->dependencyCount, .pViewOffsets = vo,
        .correlationMaskCount = sc, .pCorrelationMasks = cm,
    };
    VkRenderPassCreateInfo mod = *pCI;
    if (pa) mod.pAttachments = pa;
    mv.pNext = NULL;
    mod.pNext = &mv;
    STEREO_LOG(
        "MV_RP_CREATE subpasses=%u viewMask0=0x%x correlationMask0=0x%x",
        sc,
        sc ? vm[0] : 0,
        sc ? cm[0] : 0);
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
    STEREO_LOG("CALLED stereo_CreateRenderPass");
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    STEREO_LOG(
        "RP sd=%p enabled=%u real_device=%p",
        (void*)sd,
        sd ? sd->stereo.enabled : 0,
        sd ? (void*)sd->real_device : NULL);

    STEREO_LOG("stereo_CreateRenderPass: attachments=%u",
               pCreateInfo ? pCreateInfo->attachmentCount : 0);

    if (pCreateInfo) {
        STEREO_LOG(
            "RP_DETAIL subpasses=%u attachments=%u dependencies=%u flags=%x",
            pCreateInfo->subpassCount,
            pCreateInfo->attachmentCount,
            pCreateInfo->dependencyCount,
            pCreateInfo->flags);
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            const VkAttachmentDescription *a = &pCreateInfo->pAttachments[i];
            STEREO_LOG(
                "RP_ATT i=%u format=%u samples=%u load=%u store=%u "
                "stencilLoad=%u stencilStore=%u initial=%u final=%u flags=%x",
                i,
                a->format,
                a->samples,
                a->loadOp,
                a->storeOp,
                a->stencilLoadOp,
                a->stencilStoreOp,
                a->initialLayout,
                a->finalLayout,
                a->flags);
        }
        for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
            const VkSubpassDescription *s = &pCreateInfo->pSubpasses[i];
            STEREO_LOG(
                "RP_SUBPASS i=%u flags=%x bindPoint=%u inputs=%u colors=%u resolves=%u "
                "preserve=%u depth=%p",
                i,
                s->flags,
                s->pipelineBindPoint,
                s->inputAttachmentCount,
                s->colorAttachmentCount,
                s->pResolveAttachments ? s->colorAttachmentCount : 0,
                s->preserveAttachmentCount,
                (void*)s->pDepthStencilAttachment);
            for (uint32_t j = 0; j < s->colorAttachmentCount; j++) {
                STEREO_LOG(
                    "RP_COLOR_DETAIL subpass=%u i=%u attachment=%u format=%u samples=%u initial=%u final=%u",
                    i,
                    j,
                    s->pColorAttachments[j].attachment,
                    s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED &&
                    s->pColorAttachments[j].attachment < pCreateInfo->attachmentCount ?
                    pCreateInfo->pAttachments[s->pColorAttachments[j].attachment].format : 0,
                    s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED &&
                    s->pColorAttachments[j].attachment < pCreateInfo->attachmentCount ?
                    pCreateInfo->pAttachments[s->pColorAttachments[j].attachment].samples : 0,
                    s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED &&
                    s->pColorAttachments[j].attachment < pCreateInfo->attachmentCount ?
                    pCreateInfo->pAttachments[s->pColorAttachments[j].attachment].initialLayout : 0,
                    s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED &&
                    s->pColorAttachments[j].attachment < pCreateInfo->attachmentCount ?
                    pCreateInfo->pAttachments[s->pColorAttachments[j].attachment].finalLayout : 0);
            }
            if (s->pDepthStencilAttachment) {
                STEREO_LOG(
                    "RP_DEPTH subpass=%u attachment=%u",
                    i,
                    s->pDepthStencilAttachment->attachment);
            }
        }
    }

    /* Step 1: create ORIGINAL render pass */
    VkResult res = create_patched_rp(sd, pCreateInfo, pAllocator, pRenderPass);
    if (res != VK_SUCCESS)
        return res;

    if (sd->render_pass_count >= MAX_RENDER_PASSES)
        return VK_SUCCESS;

    CHECK_ARRAY_COUNT(sd->render_pass_count, MAX_RENDER_PASSES, "render_pass_count");
    StereoRenderPassInfo *rpi =
        &sd->render_passes[sd->render_pass_count++];

    rpi->handle        = *pRenderPass;
    rpi->mv_handle     = VK_NULL_HANDLE;
    rpi->has_multiview = false;
    rpi->view_mask     = 0;
    rpi->subpass_count = pCreateInfo->subpassCount;

    rpi->shading_rate_attachment = VK_ATTACHMENT_UNUSED;
    rpi->has_shading_rate_attachment = false;
    STEREO_LOG(
        "RP_FSR attachment=%u has=%u",
        rpi->shading_rate_attachment,
        (unsigned)rpi->has_shading_rate_attachment);

    /* Step 2: classify */
    bool depth_only = is_depth_only_renderpass(pCreateInfo);
    bool shadow_pass = is_shadow_renderpass(pCreateInfo);
    bool present_pass = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        if (pCreateInfo->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            present_pass = true;
            break;
        }
    }
    bool mv_eligible =
    sd->stereo.enabled &&
    sd->stereo.multiview &&
    pCreateInfo->attachmentCount > 0 &&
    !depth_only &&
    !shadow_pass;
    STEREO_LOG(
        "RenderPass classify: rp=%p depth_only=%d shadow_pass=%d present_pass=%d mv_eligible=%d attachments=%u subpasses=%u fmt0=%u",
        (void*)*pRenderPass,
        depth_only,
        shadow_pass,
        present_pass,
        mv_eligible,
        pCreateInfo->attachmentCount,
        pCreateInfo->subpassCount,
        pCreateInfo->attachmentCount ?
        pCreateInfo->pAttachments[0].format :
        0);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        STEREO_LOG(
            "RP_ATTACHMENT rp=%p i=%u format=%u load=%u store=%u initial=%u final=%u",
            (void*)*pRenderPass,
            i,
            pCreateInfo->pAttachments[i].format,
            pCreateInfo->pAttachments[i].loadOp,
            pCreateInfo->pAttachments[i].storeOp,
            pCreateInfo->pAttachments[i].initialLayout,
            pCreateInfo->pAttachments[i].finalLayout);
    }
    if (!mv_eligible)
    {
        STEREO_LOG(
            "RP_MV_REJECT rp=%p depth_only=%u shadow_pass=%u present_pass=%u multiview=%u enabled=%u attachments=%u subpasses=%u reason=%s",
            (void*)*pRenderPass,
            (unsigned)depth_only,
            (unsigned)shadow_pass,
            (unsigned)present_pass,
            (unsigned)sd->stereo.multiview,
            (unsigned)sd->stereo.enabled,
            pCreateInfo->attachmentCount,
            pCreateInfo->subpassCount,
            pCreateInfo->attachmentCount == 0 ? "NO_ATTACHMENTS" : depth_only ? "DEPTH_ONLY" : shadow_pass ? "SHADOW_PASS" : present_pass ? "PRESENT_PASS" : "MV_DISABLED");
        if (shadow_pass && pCreateInfo->pAttachments)
        {
            for (uint32_t a = 0; a < pCreateInfo->attachmentCount; a++)
            {
                const VkAttachmentDescription *ad = &pCreateInfo->pAttachments[a];
                STEREO_LOG(
                    "RP_SHADOW_ATTACHMENT rp=%p index=%u format=%u samples=%u load=%u store=%u initial_layout=%u final_layout=%u",
                    (void*)*pRenderPass,
                    a,
                    (unsigned)ad->format,
                    (unsigned)ad->samples,
                    (unsigned)ad->loadOp,
                    (unsigned)ad->storeOp,
                    (unsigned)ad->initialLayout,
                    (unsigned)ad->finalLayout);
            }
        }
        if (shadow_pass && pCreateInfo->pSubpasses)
        {
            for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++)
            {
                const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[s];
                STEREO_LOG(
                    "RP_SHADOW_SUBPASS rp=%p subpass=%u bind_point=%u inputs=%u colors=%u resolves=%u depth_ref=%p preserve=%u",
                    (void*)*pRenderPass,
                    s,
                    (unsigned)sp->pipelineBindPoint,
                    sp->inputAttachmentCount,
                    sp->colorAttachmentCount,
                    sp->pResolveAttachments ? sp->colorAttachmentCount : 0,
                    (void*)sp->pDepthStencilAttachment,
                    sp->preserveAttachmentCount);
            }
        }
        return VK_SUCCESS;
    }
    STEREO_LOG("RP_MV_CREATE_BEGIN rp=%p slot=%u attachments=%u subpasses=%u", (void*)*pRenderPass, sd->render_pass_count-1, pCreateInfo->attachmentCount, pCreateInfo->subpassCount);

    /*
     * FIX 2 CORE: DEDUPE MV render passes
     * Ensure 1 base RP → 1 MV RP mapping.
     */
    if (sd->render_pass_count > 1)
    {
        for (uint32_t i = 0; i < sd->render_pass_count - 1; i++)
        {
            StereoRenderPassInfo *ex = &sd->render_passes[i];

            if (ex->handle == *pRenderPass && ex->mv_handle)
            {
                rpi->mv_handle     = ex->mv_handle;
                rpi->has_multiview = true;
                rpi->view_mask     = STEREO_VIEW_MASK;
                rpi->shading_rate_attachment = ex->shading_rate_attachment;
                rpi->has_shading_rate_attachment =
                    ex->has_shading_rate_attachment;
                STEREO_LOG("RenderPass %p: reused existing mv=%p",
                           (void*)*pRenderPass,
                           (void*)ex->mv_handle);
                return VK_SUCCESS;
            }
        }
    }

    /* Create MV render pass */
    VkRenderPass mv = VK_NULL_HANDLE;

    if (create_mv_rp(sd, pCreateInfo, pAllocator, &mv) == VK_SUCCESS && mv)
    {
        rpi->mv_handle     = mv;
        STEREO_LOG(
            "RP_PAIR original=%p mv=%p",
            (void*)*pRenderPass,
            (void*)mv);
        rpi->has_multiview = true;
        STEREO_LOG(
            "RP_STORE slot=%u handle=%08x mv=%08x has_mv=%u addr=%08x",
            (unsigned)(sd->render_pass_count - 1),
            (unsigned)(uintptr_t)rpi->handle,
            (unsigned)(uintptr_t)rpi->mv_handle,
            (unsigned)rpi->has_multiview,
            (unsigned)(uintptr_t)rpi);
        rpi->view_mask     = STEREO_VIEW_MASK;

        sd->multiview_pass_exists = true;

        STEREO_LOG("RenderPass %p: original=returned mv=%p (att=%u sub=%u)",
                   (void*)*pRenderPass,
                   (void*)mv,
                   pCreateInfo->attachmentCount,
                   pCreateInfo->subpassCount);
    }
    else
    {
        STEREO_ERR("RP_MV_CREATE_FAILED rp=%p result_or_handle_invalid=1 attachments=%u subpasses=%u",
            (void*)*pRenderPass,
            pCreateInfo->attachmentCount,
            pCreateInfo->subpassCount);
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
    STEREO_LOG("CALLED stereo_CreateRenderPass2KHR");
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
    CHECK_ARRAY_COUNT(sd->render_pass_count, MAX_RENDER_PASSES, "render_pass_count");

    StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
    rpi->handle = *pRenderPass;
    rpi->mv_handle = VK_NULL_HANDLE;
    rpi->has_multiview = false;
    rpi->view_mask = 0;
    rpi->subpass_count = sc;
    rpi->shading_rate_attachment = VK_ATTACHMENT_UNUSED;
    rpi->has_shading_rate_attachment =
        find_shading_rate_attachment2(
            pCreateInfo,
            &rpi->shading_rate_attachment);
    STEREO_LOG(
        "RP2_FSR attachment=%u has=%u",
        rpi->shading_rate_attachment,
        (unsigned)rpi->has_shading_rate_attachment);

    if (sd->stereo.enabled && sd->stereo.multiview) {
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
            VkResult mv2_res = sd->real.CreateRenderPass2KHR(sd->real_device, &mv2, pAllocator, &mv);
            STEREO_LOG("RP2_MV_CREATE_RESULT rp=%p mv=%p result=%d attachments=%u subpasses=%u", (void*)*pRenderPass, (void*)mv, (int)mv2_res, ac, sc);
            if (mv2_res == VK_SUCCESS && mv) {
                rpi->mv_handle = mv; rpi->has_multiview = true;
                rpi->view_mask = STEREO_VIEW_MASK;
                sd->multiview_pass_exists = true;
                STEREO_LOG("RP2_MV_STORED rp=%p mv=%p view_mask=0x%X", (void*)*pRenderPass, (void*)mv, (unsigned)rpi->view_mask);
            }
        }
        free(pa2); free(subs); free(corr);
    }
    return VK_SUCCESS;
}
#endif

/* ── vkDestroyRenderPass ────────────────────────────────────────────────────
*
* VKD3D-Proton (and others) create and destroy render passes continuously;
* the driver then recycles the same VkRenderPass handle values.  The table
* in sd->render_passes is keyed by the app's handle, so entries MUST be
* removed on destroy.  Otherwise a recycled handle matches a stale entry
* (and the CreateRenderPass dedupe reuses its stale mv_handle), giving
* pipelines/framebuffers a multiview pass with a different layout than the
* pass the app actually described -> VK_ERROR_DEVICE_LOST.
* The paired multiview pass is destroyed together with the original.
*/
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyRenderPass(
    VkDevice                     device,
    VkRenderPass                 renderPass,
    const VkAllocationCallbacks *pAllocator)
{
    STEREO_LOG("CALLED stereo_DestroyRenderPass rp=%p", (void *)renderPass);
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd)
        return;
    if (renderPass == VK_NULL_HANDLE)
        return;

    VkRenderPass mv = VK_NULL_HANDLE;
    stereo_mutex_lock(&sd->lock);
    for (uint32_t i = 0; i < sd->render_pass_count; i++)
    {
        if (sd->render_passes[i].handle == renderPass)
        {
            mv = sd->render_passes[i].mv_handle;
            uint32_t last = --sd->render_pass_count;
            if (i != last)
                sd->render_passes[i] = sd->render_passes[last];
            memset(&sd->render_passes[last], 0, sizeof(sd->render_passes[last]));
            break;
        }
    }
    stereo_mutex_unlock(&sd->lock);

    sd->real.DestroyRenderPass(sd->real_device, renderPass, pAllocator);
    if (mv != VK_NULL_HANDLE && mv != renderPass)
        sd->real.DestroyRenderPass(sd->real_device, mv, pAllocator);
    STEREO_LOG("RP_DESTROY rp=%p mv=%p remaining=%u",
        (void *)renderPass, (void *)mv, sd->render_pass_count);
}