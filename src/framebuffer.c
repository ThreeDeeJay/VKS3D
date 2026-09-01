/*
 * framebuffer.c — Per-framebuffer multiview selection + render pass substitution
 *
 * stereo_CreateFramebuffer: if ALL attachment views are upgraded (2-layer
 *   2D_ARRAY from stereo_CreateImage at swapchain extent), create the
 *   framebuffer using rpi->mv_handle so multiview rendering is enabled
 *   only for main-scene passes (G-buffer, lighting, swapchain).
 *   Shadow maps, environment probes, and other auxiliary passes use
 *   the original (non-MV) render pass since their images are not upgraded.
 *
 * stereo_CmdBeginRenderPass: if the framebuffer was created with an mv_rp,
 *   substitute the render pass in VkRenderPassBeginInfo.
 */

#include <string.h>
#include "stereo_icd.h"

StereoDevice* stereo_device_from_command_buffer(VkCommandBuffer cb);

void remember_begin_renderpass(
    StereoDevice* sd,
    VkCommandBuffer cb,
    VkRenderPass rp,
    uint32_t flags);

StereoDevice *
stereo_device_from_command_buffer(
    VkCommandBuffer cb);

VkRenderPass lookup_bound_renderpass(
    StereoDevice* sd,
    VkCommandBuffer cb);

VkFramebuffer lookup_bound_framebuffer(
    StereoDevice* sd,
    VkCommandBuffer cb);

VkPipeline lookup_bound_pipeline(
    StereoDevice* sd,
    VkCommandBuffer cb);

static void stereo_overwrite_projection_binding(
    StereoDevice *sd,
    VkDescriptorSet ds,
    uint32_t binding);

/* ── vkCreateFramebuffer ────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateFramebuffer(
    VkDevice                        device,
    const VkFramebufferCreateInfo  *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkFramebuffer                  *pFramebuffer)
{
    STEREO_LOG("CALLED stereo_CreateFramebuffer");
    STEREO_LOG("[FB ENTRY RAW] tid=%lu pFramebuffer=%p rp=%p attachmentCount=%u",
        GetCurrentThreadId(),
        pFramebuffer,
        pCreateInfo->renderPass,
        pCreateInfo->attachmentCount);
    StereoDevice *sd = stereo_device_from_handle(device);
    STEREO_LOG(
        "FB_DEVICE sd=%p real_device=%p fb_track_count(before)=%u",
        sd,
        sd ? sd->real_device : NULL,
        sd ? sd->fb_track_count : 0);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    VkFramebufferCreateInfo fci = *pCreateInfo;
    VkRenderPass debug_original = pCreateInfo->renderPass;
    
    STEREO_LOG(
        "FB_CREATE_INFO fb=%p rp=%p width=%u height=%u layers=%u attachments=%u",
        (void*)pFramebuffer,
        (void*)pCreateInfo->renderPass,
        pCreateInfo->width,
        pCreateInfo->height,
        pCreateInfo->layers,
        pCreateInfo->attachmentCount);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        STEREO_LOG(
            "FB_CREATE_ATTACHMENT i=%u view=%p",
            i,
            (void*)pCreateInfo->pAttachments[i]);
    }
    if (debug_original == VK_NULL_HANDLE) {
        STEREO_LOG("[FATAL] upstream pCreateInfo->renderPass already NULL!");
    }

    /* CRITICAL: snapshot ORIGINAL RP before any modification */
    VkRenderPass original_rp = pCreateInfo->renderPass;
    VkRenderPass use_mv      = VK_NULL_HANDLE;
    
    /* HARD ASSERT: framebuffer created without renderPass */
    if (original_rp == VK_NULL_HANDLE) {
        STEREO_LOG("[HARD ASSERT] CreateFramebuffer received NULL renderPass fb=%p",
                   pFramebuffer);
    }

    if (sd->stereo.enabled && sd->stereo.multiview && pCreateInfo->attachmentCount > 0) {
        bool all = true;
        bool any = false;
        STEREO_LOG(
            "FB_ATTACHMENT_CLASSIFY_BEGIN rp=%p attachments=%u upgraded_views=%u",
            (void*)pCreateInfo->renderPass,
            pCreateInfo->attachmentCount,
            sd->upgraded_view_count);
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            VkImageView view = pCreateInfo->pAttachments[i];
            bool found = false;
            uint32_t found_index = UINT32_MAX;
            for (uint32_t k = 0; k < sd->upgraded_view_count; k++) {
                if (sd->upgraded_views[k] == view) {
                    found = true;
                    found_index = k;
                    break;
                }
            }
            if (found)
                any = true;
            else
                all = false;
            STEREO_LOG(
                "FB_ATTACHMENT_CLASSIFY att=%u view=%p class=%s upgraded_index=%u",
                i,
                (void*)view,
                found ? "UPGRADED" : "NORMAL",
                found ? found_index : UINT32_MAX);
        }
        StereoRenderPassInfo *rpi =
            stereo_rp_lookup(sd, pCreateInfo->renderPass);
        STEREO_LOG(
            "FB_RP_RESOLVE request=%p rpi=%p handle=%p mv=%p has_mv=%u all=%u any=%u",
            (void*)pCreateInfo->renderPass,
            (void*)rpi,
            rpi ? (void*)rpi->handle : NULL,
            rpi ? (void*)rpi->mv_handle : NULL,
            rpi ? (unsigned)rpi->has_multiview : 0,
            (unsigned)all,
            (unsigned)any);
        /*
        * CRITICAL:
        * A framebuffer may use the multiview render pass only when every
        * framebuffer attachment belongs to the upgraded attachment set.
        * "any" is diagnostic only and must never select the MV render pass.
        */
        if (rpi &&
            all &&
            rpi->mv_handle &&
            rpi->has_multiview &&
            rpi->handle == pCreateInfo->renderPass)
        {
            fci.renderPass = rpi->mv_handle;
            use_mv = rpi->mv_handle;
            STEREO_LOG(
                "FB_SET renderPass=%p all=%u any=%u attachments=%u",
                fci.renderPass,
                (unsigned)all,
                (unsigned)any,
                pCreateInfo->attachmentCount);
        }
        else
        {
            STEREO_LOG(
                "FB_MV_NOT_SELECTED rp=%p rpi=%p has_mv=%u mv=%p all_upgraded=%u any_upgraded=%u",
                (void*)pCreateInfo->renderPass,
                (void*)rpi,
                rpi ? (unsigned)rpi->has_multiview : 0,
                rpi ? (void*)rpi->mv_handle : NULL,
                (unsigned)all,
                (unsigned)any);
        }
        if (!all) 
        {
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
            {
                bool found = false;
                for (uint32_t k = 0; k < sd->upgraded_view_count; k++) 
                {
                    if (sd->upgraded_views[k] ==
                        pCreateInfo->pAttachments[i])
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    STEREO_LOG(
                        "[FB NON-UPGRADED] att=%u view=%p tracked=%u",
                        i,
                        (void*)pCreateInfo->pAttachments[i],
                        sd->upgraded_view_count);
                }
            }
        }
    }
    STEREO_LOG(
        "FB_CREATE rp_in=%p rp_used=%p mv_candidate=%p",
        (void*)pCreateInfo->renderPass,
        (void*)fci.renderPass,
        (void*)use_mv);
    STEREO_LOG(
        "FB_FINAL rp_in=%p fci.renderPass=%p use_mv=%p",
        (void*)pCreateInfo->renderPass,
        (void*)fci.renderPass,
        (void*)use_mv);
    if (fci.renderPass == VK_NULL_HANDLE && use_mv != VK_NULL_HANDLE) {
        STEREO_LOG(
            "[FATAL] renderPass was LOST during patching path original=%p mv=%p",
            (void*)debug_original,
            (void*)use_mv);
    }
    VkRenderPass before = fci.renderPass;
    STEREO_LOG(
        "FB_CALL renderPass=%p use_mv=%p original=%p",
        (void*)fci.renderPass,
        (void*)use_mv,
        (void*)original_rp);
    STEREO_LOG(
        "FB_CREATE_REAL fbCI_rp=%p orig_rp=%p",
        (void*)fci.renderPass,
        (void*)pCreateInfo->renderPass);
    VkResult res = sd->real.CreateFramebuffer(sd->real_device, &fci, pAllocator, pFramebuffer);
    if (res == VK_SUCCESS)
    {
        STEREO_LOG(
            "FB_CREATED fb=%p rp_used=%p mv=%p",
            (void*)*pFramebuffer,
            (void*)fci.renderPass,
            (void*)use_mv);
    }
    if (before != fci.renderPass) {
        STEREO_LOG(
            "[CRITICAL MUTATION] fci.renderPass changed during CreateFramebuffer: %p -> %p",
            (void*)before,
            (void*)fci.renderPass);
    }
    if (fci.renderPass == VK_NULL_HANDLE) {
        STEREO_LOG(
            "[FB_TRACK_FATAL] fci.renderPass == NULL after patching fb=%p use_mv=%p",
            (void*)*pFramebuffer,
            (void*)use_mv);
    }
    if (pCreateInfo->renderPass == VK_NULL_HANDLE) {
        STEREO_LOG(
            "[FB_TRACK_FATAL] pCreateInfo->renderPass == NULL fb=%p",
            (void*)*pFramebuffer);
    }
    if (res == VK_SUCCESS)
    {
        StereoFramebufferTrack *t;
        uint32_t idx;
        stereo_mutex_lock(&sd->lock);
        if (sd->fb_track_count >= MAX_FB_TRACK)
        {
            uint32_t count = sd->fb_track_count;
            stereo_mutex_unlock(&sd->lock);
            STEREO_LOG(
                "[FB TRACK FULL] fb=%p count=%u max=%u",
                (void*)*pFramebuffer,
                count,
                MAX_FB_TRACK);
            return res;
        }
        CHECK_ARRAY_COUNT(sd->fb_track_count, MAX_FB_TRACK, "fb_track_count");
        idx = sd->fb_track_count++;
        STEREO_LOG(
            "FB_COUNT_RESERVE idx=%u next=%u",
            idx,
            sd->fb_track_count);
        t = &sd->fb_tracks[idx];
        memset(t, 0, sizeof(*t));
        STEREO_LOG(
            "FB_LAYOUT t=%p &fb=%p &rp=%p &rp_used=%p &mv_rp=%p &has_mv=%p sizeof=%u",
            t,
            &t->fb,
            &t->rp,
            &t->rp_used_at_create,
            &t->mv_rp,
            &t->has_mv,
            (unsigned)sizeof(*t));
        t->fb = *pFramebuffer;
        t->rp = original_rp;
        if (t->rp == VK_NULL_HANDLE)
            t->rp = fci.renderPass;
        t->rp_used_at_create = fci.renderPass;
        t->mv_rp = use_mv;
        t->has_mv = (use_mv != VK_NULL_HANDLE) &&
        sd->stereo.multiview;
        STEREO_LOG(
            "FB_FIELDS rp=%p rp_used=%p mv_rp=%p has_mv=%u",
            (void*)t->rp,
            (void*)t->rp_used_at_create,
            (void*)t->mv_rp,
            (unsigned)t->has_mv);
        if (sd->stereo.enabled && sd->stereo.multiview && use_mv == VK_NULL_HANDLE)
        {
            STEREO_LOG(
                "[FB INFO] multiview enabled but use_mv == NULL fb=%p rp=%p",
                (void*)t->fb,
                (void*)t->rp);
    }
    if (use_mv != VK_NULL_HANDLE &&
        fci.renderPass == VK_NULL_HANDLE) 
    {
        STEREO_LOG(
            "[HARD ASSERT] mv_rp valid but fci.renderPass NULL fb=%p",
            (void*)t->fb);
    }
    if (use_mv != VK_NULL_HANDLE &&
        !sd->stereo.multiview)
    {
        STEREO_LOG(
            "[HARD ASSERT] mv_rp exists but stereo.multiview OFF fb=%p",
            (void*)t->fb);
    }
    STEREO_LOG(
        "FB_TRACK_CREATE idx=%u fb=%p rp=%p mv_rp=%p has_mv=%u mv_enabled=%u",
        idx,
        (void*)t->fb,
        (void*)t->rp,
        (void*)t->mv_rp,
        (unsigned)t->has_mv,
        (unsigned)sd->stereo.multiview);
    if (use_mv == VK_NULL_HANDLE) 
    {
        STEREO_LOG(
            "[FB_TRACK_WARN] MV NOT STORED fb=%p rp=%p reason=use_mv_null",
            (void*)*pFramebuffer,
            (void*)pCreateInfo->renderPass);
    }
    StereoFramebufferTrack *verify = &sd->fb_tracks[idx];
    STEREO_LOG(
        "FB_TRACK_VERIFY idx=%u fb=%p rp=%p mv_rp=%p has_mv=%u",
        idx,
        (void*)verify->fb,
        (void*)verify->rp,
        (void*)verify->mv_rp,
        (unsigned)verify->has_mv);
    stereo_mutex_unlock(&sd->lock);
    }
    return res;
}

/* ── vkDestroyFramebuffer ───────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyFramebuffer(
    VkDevice                       device,
    VkFramebuffer                  framebuffer,
    const VkAllocationCallbacks   *pAllocator)
{
    STEREO_LOG("CALLED stereo_DestroyFramebuffer");
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;
    stereo_mutex_lock(&sd->lock);
    for (uint32_t i = 0; i < sd->fb_track_count; i++)
    {
        if (sd->fb_tracks[i].fb == framebuffer)
        {
            uint32_t last = --sd->fb_track_count;
            if (i != last)
                sd->fb_tracks[i] = sd->fb_tracks[last];
            memset(&sd->fb_tracks[last], 0, sizeof(sd->fb_tracks[last]));
            STEREO_LOG(
                "FB_TRACK_DESTROY fb=%p idx=%u new_count=%u",
                (void*)framebuffer,
                i,
                sd->fb_track_count);
            break;
        }
    }
    stereo_mutex_unlock(&sd->lock);
    sd->real.DestroyFramebuffer(
        sd->real_device,
        framebuffer,
        pAllocator);
}

/* ── vkCmdBeginRenderPass ───────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_CmdBeginRenderPass(
    VkCommandBuffer              commandBuffer,
    const VkRenderPassBeginInfo *pRenderPassBegin,
    VkSubpassContents            contents)
{
    STEREO_LOG("CALLED stereo_CmdBeginRenderPass");
    extern StereoDevice g_devices[];
    extern uint32_t     g_device_count;
    StereoDevice *sd   = NULL;
    VkRenderPass mv_rp = VK_NULL_HANDLE;
    bool fb_found = false;
    bool fb_has_mv = false;
    uint32_t fb_track_index = UINT32_MAX;
    STEREO_LOG(
        "CB_BEGIN cb=%p rp=%p fb=%p",
        commandBuffer,
        pRenderPassBegin->renderPass,
        pRenderPassBegin->framebuffer);
    STEREO_LOG(
        "RP_BEGIN_ORIGINAL rp=%p fb=%p",
        (void*)pRenderPassBegin->renderPass,
        (void*)pRenderPassBegin->framebuffer);
    STEREO_LOG(
        "RP_BEGIN_SCAN device_count=%u",
        g_device_count);

    for (uint32_t d = 0; d < g_device_count; d++)
    {
        StereoDevice *dev = &g_devices[d];
        STEREO_LOG(
            "DEVICE[%u] dev=%p real_device=%p fb_track_count=%u",
            d,
            dev,
            dev->real_device,
            dev->fb_track_count);
    }
    for (uint32_t d = 0; d < g_device_count && !sd; d++) {
        StereoDevice *dev = &g_devices[d];
        STEREO_LOG(
            "FB_TRACK_SCAN dev=%p count=%u",
            dev,
            dev->fb_track_count);
        for (uint32_t i = 0; i < dev->fb_track_count; i++) {
            bool fb_match =
                (dev->fb_tracks[i].fb == pRenderPassBegin->framebuffer);
            STEREO_LOG(
                "FB_SCAN i=%u "
                "begin_fb=%p tracked_fb=%p "
                "begin_rp=%p tracked_rp=%p "
                "tracked_used=%p tracked_mv=%p "
                "has_mv=%u fb_match=%u",
                i,
                (void*)pRenderPassBegin->framebuffer,
                (void*)dev->fb_tracks[i].fb,
                (void*)pRenderPassBegin->renderPass,
                (void*)dev->fb_tracks[i].rp,
                (void*)dev->fb_tracks[i].rp_used_at_create,
                (void*)dev->fb_tracks[i].mv_rp,
                (unsigned)dev->fb_tracks[i].has_mv,
                (unsigned)fb_match);
            if (fb_match)
            {
                bool rp_match =
                (
                    dev->fb_tracks[i].rp &&
                    pRenderPassBegin->renderPass &&
                    dev->fb_tracks[i].rp == pRenderPassBegin->renderPass
                    )
                ||
                (
                    dev->fb_tracks[i].mv_rp &&
                    pRenderPassBegin->renderPass &&
                    dev->fb_tracks[i].mv_rp == pRenderPassBegin->renderPass
                    );
                fb_found = true;
                sd = dev;
                fb_track_index = i;
                fb_has_mv =
                dev->fb_tracks[i].has_mv &&
                dev->fb_tracks[i].mv_rp != VK_NULL_HANDLE;
                STEREO_LOG(
                    "FB_MATCH_CANDIDATE d=%u i=%u fb=%08x rp_begin=%08x tracked_rp=%08x mv_rp=%08x has_mv=%u rp_match=%u",
                    d,
                    i,
                    (unsigned)(uintptr_t)pRenderPassBegin->framebuffer,
                    (unsigned)(uintptr_t)pRenderPassBegin->renderPass,
                    (unsigned)(uintptr_t)dev->fb_tracks[i].rp,
                    (unsigned)(uintptr_t)dev->fb_tracks[i].mv_rp,
                    (unsigned)dev->fb_tracks[i].has_mv,
                    (unsigned)rp_match);
                STEREO_LOG(
                    "FB_TRACK_MATCH fb=%p tracked_rp=%p tracked_used=%p tracked_mv=%p has_mv=%u",
                    (void*)dev->fb_tracks[i].fb,
                    (void*)dev->fb_tracks[i].rp,
                    (void*)dev->fb_tracks[i].rp_used_at_create,
                    (void*)dev->fb_tracks[i].mv_rp,
                    (unsigned)dev->fb_tracks[i].has_mv);
                STEREO_LOG(
                    "FB_MATCH requested=%p fb_original=%p fb_used=%p fb_mv=%p",
                    (void*)pRenderPassBegin->renderPass,
                    (void*)dev->fb_tracks[i].rp,
                    (void*)dev->fb_tracks[i].rp_used_at_create,
                    (void*)dev->fb_tracks[i].mv_rp);
                if (fb_has_mv)
                {
                    mv_rp = dev->fb_tracks[i].mv_rp;
                    STEREO_LOG(
                        "MV_SELECT_FROM_FB fb=%p original_rp=%p used_rp=%p mv_rp=%p",
                        (void*)pRenderPassBegin->framebuffer,
                        (void*)dev->fb_tracks[i].rp,
                        (void*)dev->fb_tracks[i].rp_used_at_create,
                        (void*)dev->fb_tracks[i].mv_rp);
                }
                else
                {
                    STEREO_LOG(
                        "MV_NOT_SELECTED_FROM_FB fb=%p original_rp=%p used_rp=%p mv_rp=%p has_mv=%u",
                        (void*)pRenderPassBegin->framebuffer,
                        (void*)dev->fb_tracks[i].rp,
                        (void*)dev->fb_tracks[i].rp_used_at_create,
                        (void*)dev->fb_tracks[i].mv_rp,
                        (unsigned)dev->fb_tracks[i].has_mv);
                }
                break;
            }
        }
    }
    STEREO_LOG(
        "MV_AFTER_SCAN sd=%p fb_found=%u fb_has_mv=%u mv_rp=%p",
        sd,
        (unsigned)fb_found,
        (unsigned)fb_has_mv,
        (void*)mv_rp);
    if (!sd)
    {
        for (uint32_t d = 0; d < g_device_count; d++)
        {
            if (g_devices[d].real_device)
            {
                sd = &g_devices[d];
                break;
            }
        }
    }
    if (!sd) return;
    StereoRenderPassInfo *lookup =
        stereo_rp_lookup(sd, pRenderPassBegin->renderPass);
    STEREO_LOG(
        "BEGIN_RP cb=%p rp=%p mv=%u mv_handle=%p",
        (void *)commandBuffer,
        (void *)(mv_rp ? mv_rp : pRenderPassBegin->renderPass),
        lookup ? lookup->has_multiview : 0,
        lookup ? (void*)lookup->mv_handle : NULL);
    /*
    * IMPORTANT:
    * The framebuffer association and the render pass being begun must
    * both participate in MV selection.
    *
    * A tracked MV framebuffer may use its tracked MV render pass when
    * the current original render pass matches the render pass recorded
    * with the framebuffer.
    *
    * If the application begins a tracked MV framebuffer with a different
    * compatible render pass, resolve the MV variant from that requested
    * render pass.
    *
    * A tracked NON-MV framebuffer is authoritative: it must NEVER be
    * promoted to an MV render pass merely because the requested render
    * pass has an MV variant.
    *
    * RP lookup fallback is therefore permitted only for:
    *   1. a tracked framebuffer that is itself MV-capable and whose
    *      requested render pass differs from its originally tracked RP;
    *   2. an entirely untracked framebuffer.
    */
    if (fb_found)
    {
        VkRenderPass tracked_rp = sd->fb_tracks[fb_track_index].rp;
        VkRenderPass tracked_mv_rp = sd->fb_tracks[fb_track_index].mv_rp;
        bool tracked_rp_match =
        tracked_rp &&
        pRenderPassBegin->renderPass &&
        tracked_rp == pRenderPassBegin->renderPass;
        STEREO_LOG(
            "MV_TRACKED_FB_RP_CHECK fb=%p begin_rp=%p tracked_rp=%p tracked_mv=%p "
            "has_mv=%u match=%u",
            (void*)pRenderPassBegin->framebuffer,
            (void*)pRenderPassBegin->renderPass,
            (void*)tracked_rp,
            (void*)tracked_mv_rp,
            (unsigned)fb_has_mv,
            (unsigned)tracked_rp_match);
        STEREO_LOG(
            "MV_DECISION fb=%p tracked=1 tracked_rp=%p tracked_used=%p tracked_mv=%p "
            "tracked_has_mv=%u begin_rp=%p lookup_mv=%p",
            (void*)pRenderPassBegin->framebuffer,
            (void*)tracked_rp,
            (void*)sd->fb_tracks[fb_track_index].rp_used_at_create,
            (void*)tracked_mv_rp,
            (unsigned)fb_has_mv,
            (void*)pRenderPassBegin->renderPass,
            lookup ? (void*)lookup->mv_handle : NULL);
        if (fb_has_mv && tracked_rp_match)
        {
            mv_rp = tracked_mv_rp;
            STEREO_LOG(
                "MV_USE_TRACKED_FB fb=%p original_rp=%p mv_rp=%p",
                (void*)pRenderPassBegin->framebuffer,
                (void*)pRenderPassBegin->renderPass,
                (void*)mv_rp);
        }
        else if (fb_has_mv)
        {
            STEREO_LOG(
                "MV_TRACKED_FB_RP_MISMATCH fb=%p begin_rp=%p tracked_rp=%p tracked_mv=%p "
                "lookup_mv=%p",
                (void*)pRenderPassBegin->framebuffer,
                (void*)pRenderPassBegin->renderPass,
                (void*)tracked_rp,
                (void*)tracked_mv_rp,
                lookup ? (void*)lookup->mv_handle : NULL);
            if (lookup &&
                lookup->has_multiview &&
                lookup->mv_handle != VK_NULL_HANDLE)
            {
                mv_rp = lookup->mv_handle;
                STEREO_LOG(
                    "MV_USE_RP_LOOKUP_FOR_TRACKED_FB_MISMATCH fb=%p original_rp=%p lookup_mv=%p",
                    (void*)pRenderPassBegin->framebuffer,
                    (void*)pRenderPassBegin->renderPass,
                    (void*)lookup->mv_handle);
            }
            else
            {
                mv_rp = VK_NULL_HANDLE;
                STEREO_LOG(
                    "MV_BLOCKED_TRACKED_FB_RP_MISMATCH fb=%p original_rp=%p tracked_rp=%p",
                    (void*)pRenderPassBegin->framebuffer,
                    (void*)pRenderPassBegin->renderPass,
                    (void*)tracked_rp);
            }
        }
        else
        {
            mv_rp = VK_NULL_HANDLE;
            STEREO_LOG(
                "MV_BLOCKED_TRACKED_NONMV_FB fb=%p original_rp=%p tracked_rp=%p "
                "tracked_mv=%p lookup_mv=%p rp_match=%u",
                (void*)pRenderPassBegin->framebuffer,
                (void*)pRenderPassBegin->renderPass,
                (void*)tracked_rp,
                (void*)tracked_mv_rp,
                lookup ? (void*)lookup->mv_handle : NULL,
                (unsigned)tracked_rp_match);
        }
    }
    else if (lookup &&
        lookup->has_multiview &&
        lookup->mv_handle != VK_NULL_HANDLE)
    {
        mv_rp = lookup->mv_handle;
        STEREO_LOG(
            "MV_RP_FALLBACK_FROM_RP cb=%p original=%p mv=%p",
            (void *)commandBuffer,
            (void *)pRenderPassBegin->renderPass,
            (void *)mv_rp);
    }
    /* CRITICAL DIAGNOSTIC: MV expected but not resolved */
    if (mv_rp == VK_NULL_HANDLE)
    {
        STEREO_LOG(
            "MV RP NOT SELECTED fb=%p rp=%p fb_found=%u fb_has_mv=%u",
            (void*)pRenderPassBegin->framebuffer,
            (void*)pRenderPassBegin->renderPass,
            (unsigned)fb_found,
            (unsigned)fb_has_mv);
    }
    STEREO_LOG(
        "RP_BEGIN fb=%p mv_rp=%p active=%d",
        (void*)pRenderPassBegin->framebuffer,
        (void*)mv_rp,
        mv_rp != VK_NULL_HANDLE);
    if (mv_rp)
    {
        VkRenderPassBeginInfo modified = *pRenderPassBegin;
        modified.renderPass = mv_rp;
        STEREO_LOG(
            "BEGIN_PASS_DRIVER original=%p mv=%p framebuffer=%p",
            (void*)pRenderPassBegin->renderPass,
            (void*)modified.renderPass,
            (void*)modified.framebuffer);
        STEREO_LOG(
            "DXVK_RP_CORRELATE original=%p driver=%p framebuffer=%p lookup=%p lookup_orig=%p lookup_mv=%p has_mv=%u",
            (void*)pRenderPassBegin->renderPass,
            (void*)modified.renderPass,
            (void*)modified.framebuffer,
            (void*)lookup,
            lookup ? (void*)lookup->handle : NULL,
            lookup ? (void*)lookup->mv_handle : NULL,
            lookup ? lookup->has_multiview : 0);
        STEREO_LOG(
            "RP_BEGIN_DRIVER rp=%p fb=%p",
            (void*)modified.renderPass,
            (void*)modified.framebuffer);
        STEREO_LOG(
            "[RP BEGIN MV] fb=%p rp=%p mv_rp=%p",
            (void*)pRenderPassBegin->framebuffer,
            (void*)pRenderPassBegin->renderPass,
            (void*)mv_rp);
        STEREO_LOG(
            "RP_BEGIN_CORRELATE cb=%p original_rp=%p driver_rp=%p fb=%p lookup=%p "
            "lookup_orig=%p lookup_mv=%p lookup_has_mv=%u",
            (void*)commandBuffer,
            (void*)pRenderPassBegin->renderPass,
            (void*)modified.renderPass,
            (void*)modified.framebuffer,
            (void*)lookup,
            lookup ? (void*)lookup->handle : NULL,
            lookup ? (void*)lookup->mv_handle : NULL,
            lookup ? lookup->has_multiview : 0);
        for (uint32_t i = 0; i < sd->fb_track_count; i++)
        {
            if (sd->fb_tracks[i].fb == modified.framebuffer)
            {
                STEREO_LOG(
                    "RP_BEGIN_FB_TRACK cb=%p fb=%p fb_rp=%p fb_used=%p fb_mv=%p "
                    "has_mv=%u begin_orig=%p begin_driver=%p",
                    (void*)commandBuffer,
                    (void*)modified.framebuffer,
                    (void*)sd->fb_tracks[i].rp,
                    (void*)sd->fb_tracks[i].rp_used_at_create,
                    (void*)sd->fb_tracks[i].mv_rp,
                    (unsigned)sd->fb_tracks[i].has_mv,
                    (void*)pRenderPassBegin->renderPass,
                    (void*)modified.renderPass);
                break;
            }
        }
        STEREO_LOG(
            "CB_DISPATCH cb=%p sd=%p real_dev=%p",
            (void*)commandBuffer,
            (void*)sd,
            (void*)sd->real_device);
        bool cb_found = false;
        for (uint32_t i = 0; i < sd->cb_track_count; i++)
        {
            if (sd->cb_track[i].cb == commandBuffer)
            {
                sd->cb_track[i].render_pass = modified.renderPass;
                sd->cb_track[i].framebuffer = modified.framebuffer;
                cb_found = true;
                STEREO_LOG(
                    "CB_TRACK_UPDATE cb=%p rp=%p fb=%p",
                    (void*)commandBuffer,
                    (void*)modified.renderPass,
                    (void*)modified.framebuffer);
                break;
            }
        }
        if (!cb_found && sd->cb_track_count < MAX_CB_TRACK)
        {
            uint32_t idx = sd->cb_track_count++;
            sd->cb_track[idx].cb = commandBuffer;
            sd->cb_track[idx].render_pass = modified.renderPass;
            sd->cb_track[idx].framebuffer = modified.framebuffer;
            STEREO_LOG(
                "CB_TRACK_CREATE cb=%p idx=%u rp=%p fb=%p",
                (void*)commandBuffer,
                idx,
                (void*)modified.renderPass,
                (void*)modified.framebuffer);
        }
        else if (!cb_found)
        {
            STEREO_LOG(
                "CB_TRACK_OVERFLOW cb=%p count=%u max=%u",
                (void*)commandBuffer,
                sd->cb_track_count,
                MAX_CB_TRACK);
        }
        sd->real.CmdBeginRenderPass(commandBuffer, &modified, contents);
    } else {
        STEREO_LOG(
            "RP_BEGIN_DRIVER rp=%p fb=%p",
            (void*)pRenderPassBegin->renderPass,
            (void*)pRenderPassBegin->framebuffer);
        STEREO_LOG(
            "BEGIN_PASS_DRIVER original=%p framebuffer=%p",
            (void*)pRenderPassBegin->renderPass,
            (void*)pRenderPassBegin->framebuffer);
        STEREO_LOG(
            "CB_DISPATCH cb=%p sd=%p real_dev=%p",
            (void*)commandBuffer,
            (void*)sd,
            (void*)sd->real_device);
        for (uint32_t i = 0; i < sd->cb_track_count; i++)
        {
            if (sd->cb_track[i].cb == commandBuffer)
            {
                sd->cb_track[i].render_pass = pRenderPassBegin->renderPass;
                sd->cb_track[i].framebuffer = pRenderPassBegin->framebuffer;
                break;
            }
        }
        STEREO_LOG(
            "BEGIN_PASS_DRIVER original=%p framebuffer=%p",
            (void*)pRenderPassBegin->renderPass,
            (void*)pRenderPassBegin->framebuffer);
        STEREO_LOG(
            "[RP BEGIN MONO] fb=%p rp=%p",
            (void*)pRenderPassBegin->framebuffer,
            (void*)pRenderPassBegin->renderPass);
        sd->real.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdBeginRendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo *pRenderingInfo)
{
    STEREO_LOG("CALLED stereo_CmdBeginRendering cb=%p info=%p",
        (void*)commandBuffer,
        (void*)pRenderingInfo);
    STEREO_LOG("BEGIN_RENDERING STEP=1");
    extern StereoDevice g_devices[];
    extern uint32_t g_device_count;
    STEREO_LOG("BEGIN_RENDERING STEP=2 device_count=%u", g_device_count);
    StereoDevice *sd = NULL;
    for (uint32_t i = 0; i < g_device_count; i++)
    {
        STEREO_LOG("BEGIN_RENDERING STEP=3 i=%u real_device=%p",
            i,
            (void*)g_devices[i].real_device);
        if (g_devices[i].real_device)
        {
            sd = &g_devices[i];
            break;
        }
    }
    STEREO_LOG("BEGIN_RENDERING STEP=4 sd=%p", (void*)sd);
    if (!sd)
    {
        STEREO_LOG("BEGIN_RENDERING RETURN reason=no_device");
        return;
    }
    STEREO_LOG("BEGIN_RENDERING STEP=5 real_CmdBeginRendering=%p",
        (void*)sd->real.CmdBeginRendering);
    if (!sd->real.CmdBeginRendering)
    {
        STEREO_LOG("BEGIN_RENDERING RETURN reason=no_real_function");
        return;
    }
    STEREO_LOG("BEGIN_RENDERING STEP=6 viewMask=%u layerCount=%u colorCount=%u depthView=%p",
        pRenderingInfo->viewMask,
        pRenderingInfo->layerCount,
        pRenderingInfo->colorAttachmentCount,
        pRenderingInfo->pDepthAttachment ?
        (void *)(uintptr_t)pRenderingInfo->pDepthAttachment->imageView : NULL);
    if (pRenderingInfo->colorAttachmentCount &&
        pRenderingInfo->pColorAttachments)
    {
        for (uint32_t ci = 0; ci < pRenderingInfo->colorAttachmentCount; ci++)
        {
            STEREO_LOG(
                "BEGIN_RENDERING_COLOR ci=%u view=%p layout=%u loadOp=%u storeOp=%u",
                ci,
                (void *)(uintptr_t)pRenderingInfo->pColorAttachments[ci].imageView,
                pRenderingInfo->pColorAttachments[ci].imageLayout,
                pRenderingInfo->pColorAttachments[ci].loadOp,
                pRenderingInfo->pColorAttachments[ci].storeOp);
        }
    }
    if (pRenderingInfo->pDepthAttachment)
    {
        STEREO_LOG(
            "BEGIN_RENDERING_DEPTH view=%p layout=%u loadOp=%u storeOp=%u",
            (void *)(uintptr_t)pRenderingInfo->pDepthAttachment->imageView,
            pRenderingInfo->pDepthAttachment->imageLayout,
            pRenderingInfo->pDepthAttachment->loadOp,
            pRenderingInfo->pDepthAttachment->storeOp);
    }
    VkRenderingInfo modified = *pRenderingInfo;
    if (sd->stereo.multiview &&
        !sd->stereo.shader_objects_mono &&
        modified.viewMask == 0)
    {
        modified.viewMask = 0x3;
        STEREO_LOG(
            "BEGIN_RENDERING_UPGRADE viewMask 0x0->0x3 layerCount=%u colors=%u",
            modified.layerCount,
            modified.colorAttachmentCount);
    }
    STEREO_LOG(
        "BEGIN_RENDERING viewMask=0x%x layerCount=%u colors=%u flags=0x%x",
        modified.viewMask,
        modified.layerCount,
        modified.colorAttachmentCount,
        modified.flags);
    STEREO_LOG(
        "BEGIN_RENDERING FORWARD real=%p viewMask=0x%x layers=%u",
        sd->real.CmdBeginRendering,
        modified.viewMask,
        modified.layerCount);
    sd->real.CmdBeginRendering(
        commandBuffer,
        &modified);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdEndRendering(
    VkCommandBuffer commandBuffer)
{
    STEREO_LOG("CALLED stereo_CmdEndRendering");
    extern StereoDevice g_devices[];
    extern uint32_t g_device_count;
    
    StereoDevice *sd = NULL;
    
    for (uint32_t i = 0; i < g_device_count; i++)
    {
        if (g_devices[i].real_device)
        {
            sd = &g_devices[i];
            break;
        }
    }
    STEREO_LOG(
        "END_RENDERING LOOKUP sd=%p real=%p",
        (void*)sd,
        sd ? (void*)sd->real.CmdEndRendering : NULL);
    if (!sd || !sd->real.CmdEndRendering)
        return;
    STEREO_LOG("END_RENDERING");
    STEREO_LOG(
        "END_RENDERING FORWARD real=%p",
        sd->real.CmdEndRendering);
    sd->real.CmdEndRendering(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline)
{
    STEREO_LOG("CALLED stereo_CmdBindPipeline");
    extern StereoDevice g_devices[];
    extern uint32_t g_device_count;

    StereoDevice *sd = NULL;

    VkRenderPass active_rp = VK_NULL_HANDLE;
    VkFramebuffer active_fb = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < g_device_count; i++)
    {
        if (g_devices[i].real_device)
        {
            sd = &g_devices[i];
            break;
        }
    }
    if (!sd)
    return;

    for (uint32_t i = 0; i < sd->cb_track_count; i++)
    {
        if (sd->cb_track[i].cb == commandBuffer)
        {
            active_rp = sd->cb_track[i].render_pass;
            active_fb = sd->cb_track[i].framebuffer;
            break;
        }
    }
    StereoPipelineInfo *info =
        find_pipeline_info(sd, pipeline);
    remember_bound_pipeline(
        sd,
        commandBuffer,
        pipeline);
    STEREO_LOG(
        "PIPE_BIND pipe=%p current_rp=%p pipeline_mv_rp=%p pipeline_orig_rp=%p",
        (void *)pipeline,
        (void *)active_rp,
        info ? (void *)info->mv_renderpass : NULL,
        info ? (void *)info->original_renderpass : NULL);
    if (info)
    {
        STEREO_LOG(
            "PIPE_BIND pipe=%p fb=%p rp=%p mv_rp=%p "
            "orig_rp=%p patched_vs=%u patched_fs=%u "
            "quad=%u bindings=%u",
            (void*)pipeline,
            (void*)active_fb,
            (void*)active_rp,
            (void*)info->mv_renderpass,
            (void*)info->original_renderpass,
            info->patched_vs,
            info->patched_fs,
            info->is_quad,
            info->vertex_binding_count);
    }
    else
    {
        STEREO_LOG(
            "PIPE_BIND pipe=%p UNKNOWN fb=%p rp=%p",
            (void*)pipeline,
            (void*)active_fb,
            (void*)active_rp);
    }
    sd->real.CmdBindPipeline(
        commandBuffer,
        pipelineBindPoint,
        pipeline);
}

static StereoDevice *
find_any_device(void)
{
    extern StereoDevice g_devices[];
    extern uint32_t g_device_count;

    for (uint32_t i = 0; i < g_device_count; i++)
    {
        if (g_devices[i].real_device)
            return &g_devices[i];
    }

    return NULL;
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance)
{
    STEREO_LOG("CALLED stereo_CmdDrawIndexed");
    STEREO_LOG(
        "DRAW_INDEXED_ARGS cb=%p indexCount=%u instanceCount=%u "
        "firstIndex=%u vertexOffset=%d firstInstance=%u",
        (void*)commandBuffer,
        indexCount,
        instanceCount,
        firstIndex,
        vertexOffset,
        firstInstance);
    StereoDevice *sd = find_any_device();
    if (!sd)
        return;
    VkPipeline pipe =
        lookup_bound_pipeline(sd, commandBuffer);
    VkRenderPass rp =
        lookup_bound_renderpass(sd, commandBuffer);
    VkFramebuffer fb =
        lookup_bound_framebuffer(sd, commandBuffer);
    StereoPipelineInfo *info =
        find_pipeline_info(sd, pipe);
    if (info)
    {
        STEREO_LOG(
            "DRAW_INDEXED pipe=%p rp=%p fb=%p quad=%u patched_vs=%u patched_fs=%u cb=%p "
            "indexCount=%u instanceCount=%u firstIndex=%u vertexOffset=%d firstInstance=%u",
            (void *)pipe,
            (void *)rp,
            (void *)fb,
            info->is_quad,
            info->patched_vs,
            info->patched_fs,
            (void *)commandBuffer,
            indexCount,
            instanceCount,
            firstIndex,
            vertexOffset,
            firstInstance);
    }
    else
    {
        STEREO_LOG(
            "DRAW_INDEXED pipe=%p UNKNOWN cb=%p "
            "indexCount=%u instanceCount=%u firstIndex=%u vertexOffset=%d firstInstance=%u",
            (void *)pipe,
            (void *)commandBuffer,
            indexCount,
            instanceCount,
            firstIndex,
            vertexOffset,
            firstInstance);
    }
    sd->real.CmdDrawIndexed(
        commandBuffer,
        indexCount,
        instanceCount,
        firstIndex,
        vertexOffset,
        firstInstance);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance)
{
    STEREO_LOG("CALLED stereo_CmdDraw");
    StereoDevice *sd = find_any_device();
    if (!sd)
        return;
    VkPipeline pipe =
        lookup_bound_pipeline(sd, commandBuffer);
    VkRenderPass rp =
        lookup_bound_renderpass(sd, commandBuffer);
    VkFramebuffer fb =
        lookup_bound_framebuffer(sd, commandBuffer);
    StereoPipelineInfo *info =
        find_pipeline_info(sd, pipe);
    if (info)
    {
        STEREO_LOG(
            "DRAW pipe=%p rp=%p fb=%p quad=%u patched_vs=%u patched_fs=%u "
            "verts=%u inst=%u",
            (void *)pipe,
            (void *)rp,
            (void *)fb,
            info->is_quad,
            info->patched_vs,
            info->patched_fs,
            vertexCount,
            instanceCount);
    }
    else
    {
        STEREO_LOG(
            "DRAW pipe=%p UNKNOWN",
            (void *)pipe);
    }
    STEREO_LOG(
        "DRAW_STEREO cmd=%p",
        (void*)commandBuffer);
    sd->real.CmdDraw(
        commandBuffer,
        vertexCount,
        instanceCount,
        firstVertex,
        firstInstance);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdDrawIndirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride)
{
    STEREO_LOG("CALLED stereo_CmdDrawIndirect");
    StereoDevice *sd = find_any_device();
    if (!sd)
        return;
    VkPipeline pipe =
        lookup_bound_pipeline(sd, commandBuffer);
    VkRenderPass rp =
        lookup_bound_renderpass(sd, commandBuffer);
    VkFramebuffer fb =
        lookup_bound_framebuffer(sd, commandBuffer);
    StereoPipelineInfo *info =
        find_pipeline_info(sd, pipe);
    if (info)
    {
        STEREO_LOG(
            "DRAW_INDIRECT pipe=%p rp=%p fb=%p quad=%u patched_vs=%u patched_fs=%u "
            "draws=%u",
            (void *)pipe,
            (void *)rp,
            (void *)fb,
            info->is_quad,
            info->patched_vs,
            info->patched_fs,
            drawCount);
    }
    else
    {
        STEREO_LOG(
            "DRAW_INDIRECT pipe=%p UNKNOWN",
            (void *)pipe);
    }
    sd->real.CmdDrawIndirect(
        commandBuffer,
        buffer,
        offset,
        drawCount,
        stride);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdDrawIndexedIndirect(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride)
{
    STEREO_LOG("CALLED stereo_CmdDrawIndexedIndirect");
    StereoDevice *sd = find_any_device();
    if (!sd)
        return;
    VkPipeline pipe =
        lookup_bound_pipeline(sd, commandBuffer);
    VkRenderPass rp =
        lookup_bound_renderpass(sd, commandBuffer);
    VkFramebuffer fb =
        lookup_bound_framebuffer(sd, commandBuffer);
    StereoPipelineInfo *info =
        find_pipeline_info(sd, pipe);
    if (info)
    {
    STEREO_LOG(
        "DRAW_INDEXED_INDIRECT pipe=%p rp=%p fb=%p quad=%u patched_vs=%u patched_fs=%u "
        "draws=%u",
        (void *)pipe,
        (void *)rp,
        (void *)fb,
        info->is_quad,
        info->patched_vs,
        info->patched_fs,
        drawCount);
    }
    else
    {
        STEREO_LOG(
            "DRAW_INDEXED_INDIRECT pipe=%p UNKNOWN",
            (void *)pipe);
    }
    sd->real.CmdDrawIndexedIndirect(
        commandBuffer,
        buffer,
        offset,
        drawCount,
        stride);
}


static void stereo_overwrite_projection_binding(
    StereoDevice *sd,
    VkDescriptorSet set,
    uint32_t binding)
{
    if (!sd || !sd->stereo.enabled || !sd->stereo_ubo_map || set == VK_NULL_HANDLE)
        return;
    StereoUBO *ubo = (StereoUBO *)sd->stereo_ubo_map;
    if (!ubo)
        return;
    STEREO_LOG(
        "DESC_REWRITE_BEGIN "
        "set=%p "
        "binding=%u "
        "ubo=%p",
        (void *)(uintptr_t)set,
        binding,
        (void *)(uintptr_t)sd->stereo_ubo);
    VkDescriptorBufferInfo bi = {
        .buffer = sd->stereo_ubo,
        .offset = 0,
        .range  = sizeof(StereoUBO),
    };
    VkWriteDescriptorSet w = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = set,
        .dstBinding      = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo     = &bi,
    };
    sd->real.UpdateDescriptorSets(sd->real_device, 1, &w, 0, NULL);
    STEREO_LOG(
        "DESC_REWRITE_DONE "
        "set=%p "
        "binding=%u "
        "buffer=%p",
        (void *)(uintptr_t)set,
        binding,
        (void *)(uintptr_t)sd->stereo_ubo);
}

VKAPI_ATTR void VKAPI_CALL
stereo_UpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet *pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet *pDescriptorCopies)
{
    STEREO_LOG("CALLED stereo_UpdateDescriptorSets");
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd)
        return;
    for (uint32_t i = 0; i < descriptorWriteCount; i++)
    {
        const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
        if (!w->pImageInfo)
            continue;
        if (w->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            w->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            w->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            continue;
        for (uint32_t j = 0; j < w->descriptorCount; j++)
        {
            VkImageView view = w->pImageInfo[j].imageView;
            STEREO_LOG(
                "DESC_WRITE binding=%u view=%p layout=%u type=%u",
                w->dstBinding,
                (void *)(uintptr_t)view,
                w->pImageInfo[j].imageLayout,
                w->descriptorType);
            bool upgraded = false;
            for (uint32_t k = 0;
                 k < sd->upgraded_view_count;
                 k++)
            {
                if (sd->upgraded_views[k] == view)
                {
                    upgraded = true;
                    break;
                }
            }
            STEREO_LOG(
                "DESC_IMAGE_WRITE "
                "binding=%u "
                "view=%p "
                "upgraded=%u",
                w->dstBinding,
                (void *)(uintptr_t)view,
                upgraded ? 1 : 0);
        }
    }
    sd->real.UpdateDescriptorSets(
        sd->real_device,
        descriptorWriteCount,
        pDescriptorWrites,
        descriptorCopyCount,
        pDescriptorCopies);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t *pDynamicOffsets)
{
    STEREO_LOG("CALLED stereo_CmdBindDescriptorSets");
    StereoDevice *sd = find_any_device();
    if (!sd)
        return;
    if (sd->stereo.enabled &&
        pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS &&
        pDescriptorSets &&
        descriptorSetCount > 0)
    {
        VkPipeline pipe = lookup_bound_pipeline(sd, commandBuffer);
        StereoPipelineInfo *info = find_pipeline_info(sd, pipe);
        if (info &&
            info->has_proj_ubo &&
            info->proj_set != UINT32_MAX &&
            info->proj_binding != UINT32_MAX &&
            info->proj_member_mask != UINT32_MAX &&
            info->proj_var != UINT32_MAX)
        {
            uint32_t target_set = info->proj_set;
            STEREO_LOG(
                "PROJ_PIPE pipe=%p set=%u binding=%u mask=0x%X var=%u",
                (void *)pipe,
                info->proj_set,
                info->proj_binding,
                info->proj_member_mask,
                info->proj_var);
            if (target_set >= firstSet &&
                target_set < firstSet + descriptorSetCount)
            {
                uint32_t rel = target_set - firstSet;
                VkDescriptorSet ds = pDescriptorSets[rel];
                STEREO_LOG(
                    "PIPE_PROJ_DS pipe=%p targetSet=%u rel=%u ds=%p",
                    (void*)pipe,
                    target_set,
                    rel,
                    (void*)ds);
                if (ds != VK_NULL_HANDLE)
                {
                    STEREO_LOG(
                        "PROJ_BIND_CANDIDATE pipe=%p firstSet=%u setCount=%u targetSet=%u "
                        "binding=%u member_mask=0x%x dynamicOffsetCount=%u",
                        (void *)pipe,
                        firstSet,
                        descriptorSetCount,
                        target_set,
                        info->proj_binding,
                        info->proj_member_mask,
                        dynamicOffsetCount);
                    if (dynamicOffsetCount > 0 && pDynamicOffsets)
                    {
                        STEREO_LOG(
                            "PROJ_BIND_DYNAMIC_OFFSETS pipe=%p first=%u count=%u",
                            (void *)pipe,
                            pDynamicOffsets[0],
                            dynamicOffsetCount);
                    }
                    STEREO_LOG(
                        "PROJ_REWRITE_CHECK pipe=%p has=%u set=%u binding=%u mask=0x%X var=%u",
                        (void *)pipe,
                        info->has_proj_ubo,
                        info->proj_set,
                        info->proj_binding,
                        info->proj_member_mask,
                        info->proj_var);
                    bool rewrite_proj = false;
                    STEREO_LOG(
                        "PROJ_REWRITE_DECISION pipe=%p rewrite=%u binding=%u mask=0x%X set=%u",
                        (void *)pipe,
                        rewrite_proj ? 1 : 0,
                        info->proj_binding,
                        info->proj_member_mask,
                        info->proj_set);
                    if (rewrite_proj)
                    {
                        stereo_write_ubo(sd);
                        STEREO_LOG(
                            "PIPE_PROJ_REWRITE_BEGIN binding=%u ds=%p",
                            info->proj_binding,
                            (void*)ds);
                        stereo_overwrite_projection_binding(
                            sd,
                            ds,
                            info->proj_binding);
                        STEREO_LOG(
                            "PROJ_BIND_REWRITE set=%p binding=%u buffer=%p member_mask=0x%x",
                            (void *)(uintptr_t)ds,
                            info->proj_binding,
                            (void *)sd->stereo_ubo,
                            info->proj_member_mask);
                        STEREO_LOG(
                            "PIPE_PROJ_REWRITE_END pipe=%p set=%u binding=%u ds=%p mask=0x%X",
                            (void *)pipe,
                            target_set,
                            info->proj_binding,
                            (void *)ds,
                            info->proj_member_mask);
                    }
                    else
                    {
                        STEREO_LOG(
                            "PROJ_DESC_SKIP pipe=%p set=%u binding=%u ds=%p mask=0x%X",
                            (void *)pipe,
                            target_set,
                            info->proj_binding,
                            (void *)ds,
                            info->proj_member_mask);
                    }
                }
            }
        }
        else if (info)
        {
            STEREO_LOG(
                "PIPE_PROJ_META pipe=%p has_proj=%u set=%u binding=%u mask=0x%X var=%u",
                (void*)pipe,
                info->has_proj_ubo,
                info->proj_set,
                info->proj_binding,
                info->proj_member_mask,
                info->proj_var);
            STEREO_LOG(
                "PROJ_REWRITE_SKIP pipe=%p has=%u set=%u binding=%u mask=0x%X var=%u",
                (void *)pipe,
                info->has_proj_ubo,
                info->proj_set,
                info->proj_binding,
                info->proj_member_mask,
                info->proj_var);
        }
    }
    STEREO_LOG(
        "BIND_DESC cmd=%p pipe=%p info=%p firstSet=%u setCount=%u set0=%p",
        (void*)commandBuffer,
        (void*)(uintptr_t)layout,
        (void*)pDescriptorSets,
        firstSet,
        descriptorSetCount,
        descriptorSetCount && pDescriptorSets
            ? (void*)(uintptr_t)pDescriptorSets[0]
            : NULL);
    sd->real.CmdBindDescriptorSets(
        commandBuffer,
        pipelineBindPoint,
        layout,
        firstSet,
        descriptorSetCount,
        pDescriptorSets,
        dynamicOffsetCount,
        pDynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindShadersEXT(
    VkCommandBuffer commandBuffer,
    uint32_t stageCount,
    const VkShaderStageFlagBits *pStages,
    const VkShaderEXT *pShaders)
{
    STEREO_LOG("CALLED stereo_CmdBindShadersEXT cb=%p count=%u",
        (void*)commandBuffer,
        stageCount);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdBindShadersEXT)
        return;
    for (uint32_t i = 0; i < stageCount; i++)
    {
        STEREO_LOG(
            "SHADER_OBJECT_BIND i=%u cb=%p stage=0x%x shader=%p",
            i,
            (void*)commandBuffer,
            pStages ? pStages[i] : 0,
            pShaders ? (void*)(uintptr_t)pShaders[i] : NULL);
    }
    STEREO_LOG(
        "SHADER_OBJECT_BIND_FORWARD cb=%p real=%p count=%u",
        (void*)commandBuffer,
        (void*)sd->real.CmdBindShadersEXT,
        stageCount);
    sd->real.CmdBindShadersEXT(
        commandBuffer,
        stageCount,
        pStages,
        pShaders);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetViewport(
    VkCommandBuffer commandBuffer,
    uint32_t firstViewport,
    uint32_t viewportCount,
    const VkViewport *pViewports)
{
    STEREO_LOG("CMD_SET_VIEWPORT cb=%p first=%u count=%u",
        (void*)commandBuffer,
        firstViewport,
        viewportCount);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetViewport)
        return;
    sd->real.CmdSetViewport(
        commandBuffer,
        firstViewport,
        viewportCount,
        pViewports);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetScissor(
    VkCommandBuffer commandBuffer,
    uint32_t firstScissor,
    uint32_t scissorCount,
    const VkRect2D *pScissors)
{
    STEREO_LOG("CMD_SET_SCISSOR cb=%p first=%u count=%u",
        (void*)commandBuffer,
        firstScissor,
        scissorCount);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetScissor)
        return;
    sd->real.CmdSetScissor(
        commandBuffer,
        firstScissor,
        scissorCount,
        pScissors);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetCullMode(
    VkCommandBuffer commandBuffer,
    VkCullModeFlags cullMode)
{
    STEREO_LOG("CMD_SET_CULL_MODE cb=%p mode=0x%x",
        (void*)commandBuffer,
        cullMode);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetCullMode)
        return;
    sd->real.CmdSetCullMode(commandBuffer, cullMode);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetFrontFace(
    VkCommandBuffer commandBuffer,
    VkFrontFace frontFace)
{
    STEREO_LOG("CMD_SET_FRONT_FACE cb=%p frontFace=%u",
        (void*)commandBuffer,
        frontFace);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetFrontFace)
        return;
    sd->real.CmdSetFrontFace(commandBuffer, frontFace);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetPrimitiveTopology(
    VkCommandBuffer commandBuffer,
    VkPrimitiveTopology primitiveTopology)
{
    STEREO_LOG("CMD_SET_PRIMITIVE_TOPOLOGY cb=%p topology=%u",
        (void*)commandBuffer,
        primitiveTopology);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetPrimitiveTopology)
        return;
    sd->real.CmdSetPrimitiveTopology(commandBuffer, primitiveTopology);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetViewportWithCountEXT(
    VkCommandBuffer commandBuffer,
    uint32_t viewportCount,
    const VkViewport *pViewports)
{
    STEREO_LOG("CMD_SET_VIEWPORT_WITH_COUNT_EXT cb=%p count=%u",
        (void*)commandBuffer,
        viewportCount);
    for (uint32_t i = 0; i < viewportCount; i++)
    {
        STEREO_LOG(
            "CMD_SET_VIEWPORT_WITH_COUNT_EXT_VALUE cb=%p i=%u "
            "x=%f y=%f w=%f h=%f minDepth=%f maxDepth=%f",
            (void*)commandBuffer,
            i,
            pViewports[i].x,
            pViewports[i].y,
            pViewports[i].width,
            pViewports[i].height,
            pViewports[i].minDepth,
            pViewports[i].maxDepth);
    }
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetViewportWithCountEXT)
        return;
    sd->real.CmdSetViewportWithCountEXT(
        commandBuffer,
        viewportCount,
        pViewports);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetScissorWithCountEXT(
    VkCommandBuffer commandBuffer,
    uint32_t scissorCount,
    const VkRect2D *pScissors)
{
    STEREO_LOG("CMD_SET_SCISSOR_WITH_COUNT_EXT cb=%p count=%u",
        (void*)commandBuffer,
        scissorCount);
    for (uint32_t i = 0; i < scissorCount; i++)
    {
        STEREO_LOG(
            "CMD_SET_SCISSOR_WITH_COUNT_EXT_VALUE cb=%p i=%u "
            "x=%d y=%d width=%u height=%u",
            (void*)commandBuffer,
            i,
            pScissors[i].offset.x,
            pScissors[i].offset.y,
            pScissors[i].extent.width,
            pScissors[i].extent.height);
    }
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetScissorWithCountEXT)
        return;
    sd->real.CmdSetScissorWithCountEXT(
        commandBuffer,
        scissorCount,
        pScissors);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthTestEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 depthTestEnable)
{
    STEREO_LOG("CMD_SET_DEPTH_TEST_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        depthTestEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetDepthTestEnableEXT)
        return;
    sd->real.CmdSetDepthTestEnableEXT(
        commandBuffer,
        depthTestEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthWriteEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 depthWriteEnable)
{
    STEREO_LOG("CMD_SET_DEPTH_WRITE_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        depthWriteEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetDepthWriteEnableEXT)
        return;
    sd->real.CmdSetDepthWriteEnableEXT(
        commandBuffer,
        depthWriteEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthCompareOpEXT(
    VkCommandBuffer commandBuffer,
    VkCompareOp depthCompareOp)
{
    STEREO_LOG("CMD_SET_DEPTH_COMPARE_OP_EXT cb=%p op=%u",
        (void*)commandBuffer,
        depthCompareOp);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetDepthCompareOpEXT)
        return;
    sd->real.CmdSetDepthCompareOpEXT(
        commandBuffer,
        depthCompareOp);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetRasterizerDiscardEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 rasterizerDiscardEnable)
{
    STEREO_LOG("CMD_SET_RASTERIZER_DISCARD_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        rasterizerDiscardEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetRasterizerDiscardEnableEXT)
        return;
    sd->real.CmdSetRasterizerDiscardEnableEXT(
        commandBuffer,
        rasterizerDiscardEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetPolygonModeEXT(
    VkCommandBuffer commandBuffer,
    VkPolygonMode polygonMode)
{
    STEREO_LOG("CMD_SET_POLYGON_MODE_EXT cb=%p mode=%u",
        (void*)commandBuffer,
        polygonMode);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetPolygonModeEXT)
        return;
    sd->real.CmdSetPolygonModeEXT(
        commandBuffer,
        polygonMode);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetRasterizationSamplesEXT(
    VkCommandBuffer commandBuffer,
    VkSampleCountFlagBits rasterizationSamples)
{
    STEREO_LOG("CMD_SET_RASTERIZATION_SAMPLES_EXT cb=%p samples=%u",
        (void*)commandBuffer,
        rasterizationSamples);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetRasterizationSamplesEXT)
        return;
    sd->real.CmdSetRasterizationSamplesEXT(
        commandBuffer,
        rasterizationSamples);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetAlphaToCoverageEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 alphaToCoverageEnable)
{
    STEREO_LOG("CMD_SET_ALPHA_TO_COVERAGE_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        alphaToCoverageEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetAlphaToCoverageEnableEXT)
        return;
    sd->real.CmdSetAlphaToCoverageEnableEXT(
        commandBuffer,
        alphaToCoverageEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthBiasEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 depthBiasEnable)
{
    STEREO_LOG("CMD_SET_DEPTH_BIAS_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        depthBiasEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetDepthBiasEnableEXT)
        return;
    sd->real.CmdSetDepthBiasEnableEXT(
        commandBuffer,
        depthBiasEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetStencilTestEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 stencilTestEnable)
{
    STEREO_LOG("CMD_SET_STENCIL_TEST_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        stencilTestEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetStencilTestEnableEXT)
        return;
    sd->real.CmdSetStencilTestEnableEXT(
        commandBuffer,
        stencilTestEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetPrimitiveRestartEnableEXT(
    VkCommandBuffer commandBuffer,
    VkBool32 primitiveRestartEnable)
{
    STEREO_LOG("CMD_SET_PRIMITIVE_RESTART_ENABLE_EXT cb=%p enable=%u",
        (void*)commandBuffer,
        primitiveRestartEnable);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetPrimitiveRestartEnableEXT)
        return;
    sd->real.CmdSetPrimitiveRestartEnableEXT(
        commandBuffer,
        primitiveRestartEnable);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetSampleMaskEXT(
    VkCommandBuffer commandBuffer,
    VkSampleCountFlagBits samples,
    const VkSampleMask *pSampleMask)
{
    STEREO_LOG("CMD_SET_SAMPLE_MASK_EXT cb=%p samples=%u",
        (void*)commandBuffer,
        samples);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetSampleMaskEXT)
        return;
    sd->real.CmdSetSampleMaskEXT(
        commandBuffer,
        samples,
        pSampleMask);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetColorBlendEnableEXT(
    VkCommandBuffer commandBuffer,
    uint32_t firstAttachment,
    uint32_t attachmentCount,
    const VkBool32 *pColorBlendEnables)
{
    STEREO_LOG("CMD_SET_COLOR_BLEND_ENABLE_EXT cb=%p first=%u count=%u",
        (void*)commandBuffer,
        firstAttachment,
        attachmentCount);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetColorBlendEnableEXT)
        return;
    sd->real.CmdSetColorBlendEnableEXT(
        commandBuffer,
        firstAttachment,
        attachmentCount,
        pColorBlendEnables);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetColorWriteMaskEXT(
    VkCommandBuffer commandBuffer,
    uint32_t firstAttachment,
    uint32_t attachmentCount,
    const VkColorComponentFlags *pColorWriteMasks)
{
    STEREO_LOG("CMD_SET_COLOR_WRITE_MASK_EXT cb=%p first=%u count=%u",
        (void*)commandBuffer,
        firstAttachment,
        attachmentCount);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetColorWriteMaskEXT)
        return;
    sd->real.CmdSetColorWriteMaskEXT(
        commandBuffer,
        firstAttachment,
        attachmentCount,
        pColorWriteMasks);
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetVertexInputEXT(
    VkCommandBuffer commandBuffer,
    uint32_t vertexBindingDescriptionCount,
    const VkVertexInputBindingDescription2EXT *pVertexBindingDescriptions,
    uint32_t vertexAttributeDescriptionCount,
    const VkVertexInputAttributeDescription2EXT *pVertexAttributeDescriptions)
{
    STEREO_LOG("CMD_SET_VERTEX_INPUT_EXT cb=%p bindings=%u attributes=%u",
        (void*)commandBuffer,
        vertexBindingDescriptionCount,
        vertexAttributeDescriptionCount);
    StereoDevice *sd = find_any_device();
    if (!sd || !sd->real.CmdSetVertexInputEXT)
        return;
    sd->real.CmdSetVertexInputEXT(
        commandBuffer,
        vertexBindingDescriptionCount,
        pVertexBindingDescriptions,
        vertexAttributeDescriptionCount,
        pVertexAttributeDescriptions);
}