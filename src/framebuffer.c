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

/* ── vkCreateFramebuffer ────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateFramebuffer(
    VkDevice                        device,
    const VkFramebufferCreateInfo  *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkFramebuffer                  *pFramebuffer)
{
    STEREO_LOG("[FB ENTRY RAW] pFramebuffer=%p rp=%p attachmentCount=%u",
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
        /* All attachments must be in sd->upgraded_views[] (2-layer 2D_ARRAY) */
        bool all = true;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount && all; i++) {
            bool found = false;
            for (uint32_t k = 0; k < sd->upgraded_view_count && !found; k++)
                if (sd->upgraded_views[k] == pCreateInfo->pAttachments[i]) found = true;
            if (!found) all = false;
        }
        if (all) {
            StereoRenderPassInfo *rpi = stereo_rp_lookup(sd, pCreateInfo->renderPass);
        if (rpi &&
            rpi->mv_handle && (rpi->handle == pCreateInfo->renderPass))
            {
                fci.renderPass = rpi->mv_handle;
                STEREO_LOG(
                    "FB_SET renderPass=%p",
                    fci.renderPass);
                use_mv         = rpi->mv_handle;
                STEREO_LOG("CreateFramebuffer: all %u att upgraded → mv_rp=%p",
                           pCreateInfo->attachmentCount, (void*)use_mv);
            }
        } else {
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                bool found = false;
        
                for (uint32_t k = 0;
                     k < sd->upgraded_view_count;
                     k++)
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
                    STEREO_LOG("[FB NON-UPGRADED] att=%u view=%p tracked=%u",
                               i,
                               pCreateInfo->pAttachments[i],
                               sd->upgraded_view_count);
                }
            }
        }
    }
    STEREO_LOG(
        "FB_CREATE rp_in=%p rp_used=%p mv_candidate=%p",
        pCreateInfo->renderPass,
        fci.renderPass,
        use_mv);
    STEREO_LOG(
        "FB_FINAL rp_in=%p fci.renderPass=%p use_mv=%p",
        pCreateInfo->renderPass,
        fci.renderPass,
        use_mv);
    if (fci.renderPass == VK_NULL_HANDLE && use_mv != VK_NULL_HANDLE) {
        STEREO_LOG("[FATAL] renderPass was LOST during patching path original=%p mv=%p",
                   debug_original, use_mv);
    }
    VkRenderPass before = fci.renderPass;
    STEREO_LOG(
        "FB_CALL renderPass=%p use_mv=%p original=%p",
        fci.renderPass,
        use_mv,
        original_rp);
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
        STEREO_LOG("[CRITICAL MUTATION] fci.renderPass changed during CreateFramebuffer: %p -> %p",
                   before, fci.renderPass);
    }
    if (fci.renderPass == VK_NULL_HANDLE)
    {
        STEREO_LOG("[FB_TRACK_FATAL] fci.renderPass == NULL after patching fb=%p use_mv=%p",
                   *pFramebuffer,
                   use_mv);
    }
    if (pCreateInfo->renderPass == VK_NULL_HANDLE)
    {
        STEREO_LOG("[FB_TRACK_FATAL] pCreateInfo->renderPass == NULL fb=%p", *pFramebuffer);
    }
    if (res == VK_SUCCESS && sd->fb_track_count < MAX_FB_TRACK)
    {
        STEREO_LOG(
            "FB_CREATE_TRACK fb=%p original=%p used=%p mv=%p",
            (void*)*pFramebuffer,
            (void*)original_rp,
            (void*)fci.renderPass,
            (void*)use_mv);
        uint32_t idx = sd->fb_track_count;

        /* IMPORTANT: snapshot BEFORE increment */
        StereoFramebufferTrack *t = &sd->fb_tracks[idx];
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
        
        /*
         * Never propagate a NULL render pass into framebuffer tracking.
         * Preserve the application's RP if present, otherwise fall back to
         * whatever CreateFramebuffer actually received.
         */
        VkRenderPass tmp_rp =
            (original_rp != VK_NULL_HANDLE) ? original_rp : fci.renderPass;
        VkRenderPass tmp_used = fci.renderPass;
        VkRenderPass tmp_mv   = use_mv;

        t->rp = tmp_rp;
        t->rp_used_at_create = tmp_used;
        t->mv_rp = tmp_mv;

        STEREO_LOG(
            "FB_FIELDS rp=%p rp_used=%p mv_rp=%p",
            t->rp,
            t->rp_used_at_create,
            t->mv_rp);
        VkRenderPass log_rp      = t->rp;
        VkRenderPass log_used    = t->rp_used_at_create;
        VkRenderPass log_mv      = t->mv_rp;
        VkFramebuffer log_fb     = t->fb;
        STEREO_LOG(
            "FB_LOCALS A=%p B=%p C=%p D=%p",
            (void*)log_rp,
            (void*)log_used,
            (void*)log_mv,
            (void*)log_fb);
        STEREO_LOG(
            "FB_ASSIGN A=%p B=%p C=%p D=%p",
            (void*)log_rp,
            (void*)log_used,
            (void*)log_mv,
            (void*)log_fb);

        {
            const unsigned char *b = (const unsigned char *)t;
            STEREO_LOG(
                "FB_BYTES "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x "
                "%02x %02x %02x %02x",
                b[0],  b[1],  b[2],  b[3],
                b[4],  b[5],  b[6],  b[7],
                b[8],  b[9],  b[10], b[11],
                b[12], b[13], b[14], b[15],
                b[16], b[17], b[18], b[19],
                b[20], b[21], b[22], b[23],
                b[24], b[25], b[26], b[27],
                b[28], b[29], b[30], b[31]);
        }

        /* HARD ASSERT: final framebuffer consistency */
        if (sd->stereo.enabled && sd->stereo.multiview) {
            if (use_mv == VK_NULL_HANDLE) {
                STEREO_LOG("[HARD ASSERT] multiview enabled but NO mv_rp resolved fb=%p rp=%p",
                           t->fb, t->rp);
            }
        
            if (use_mv != VK_NULL_HANDLE && fci.renderPass == VK_NULL_HANDLE) {
                STEREO_LOG("[HARD ASSERT] mv_rp exists but fci.renderPass lost fb=%p",
                           t->fb);
            }
        }

        /* ================= HARD ASSERT SECTION ================= */
        if (sd->stereo.enabled && sd->stereo.multiview && use_mv == VK_NULL_HANDLE) {
            STEREO_LOG("[HARD ASSERT] multiview enabled but use_mv == NULL fb=%p rp=%p",
                       t->fb, t->rp);
        }
        
        if (use_mv != VK_NULL_HANDLE && fci.renderPass == VK_NULL_HANDLE) {
            STEREO_LOG("[HARD ASSERT] mv_rp valid but fci.renderPass NULL fb=%p",
                       t->fb);
        }
        
        if (use_mv != VK_NULL_HANDLE && !sd->stereo.multiview) {
            STEREO_LOG("[HARD ASSERT] mv_rp exists but stereo.multiview OFF fb=%p",
                       t->fb);
        }
        /* ======================================================= */

        STEREO_LOG(
            "MV_BOOL_CHECK multiview=%d",
            (int)sd->stereo.multiview);
        STEREO_LOG(
            "FB_ADDR_CHECK sd=%p stereo=%p fb_tracks=%p track=%p",
            sd,
            &sd->stereo,
            sd->fb_tracks,
            t);
        STEREO_LOG(
            "FB_BOOL_CHECK multiview=%d use_mv=%p",
            (int)sd->stereo.multiview,
            use_mv);
        STEREO_LOG(
            "FB_RAW_VALUES fb=%08x rp=%08x mv=%08x",
            (unsigned)(uintptr_t)t->fb,
            (unsigned)(uintptr_t)t->rp,
            (unsigned)(uintptr_t)t->mv_rp);
        t->has_mv = (use_mv != VK_NULL_HANDLE) &&
                    sd->stereo.multiview;
        /* ===== FINAL CONSISTENCY CHECK ===== */
        if (t->has_mv && use_mv == VK_NULL_HANDLE) {
            STEREO_LOG("[HARD ASSERT] has_mv=1 but use_mv NULL fb=%p", t->fb);
        }
        
        if (!t->has_mv && use_mv != VK_NULL_HANDLE && sd->stereo.multiview) {
            STEREO_LOG("[HARD ASSERT] mv exists but has_mv=0 fb=%p", t->fb);
        }
        STEREO_LOG(
            "FB_TRACK_CREATE idx=%u fb=%08x rp=%08x mv_rp=%08x has_mv=%u mv_enabled=%u",
            idx,
            (unsigned)(uintptr_t)t->fb,
            (unsigned)(uintptr_t)t->rp,
            (unsigned)(uintptr_t)t->mv_rp,
            (unsigned)t->has_mv,
            (unsigned)sd->stereo.multiview);
        if (use_mv == VK_NULL_HANDLE)
        {
            STEREO_LOG(
                "[FB_TRACK_WARN] MV NOT STORED fb=%p rp=%p reason=use_mv_null",
                *pFramebuffer,
                pCreateInfo->renderPass);
        }
        sd->fb_track_count++;
        StereoFramebufferTrack *verify =
            &sd->fb_tracks[idx];
        STEREO_LOG(
            "FB_TRACK_VERIFY idx=%u fb=%08x rp=%08x mv_rp=%08x has_mv=%u",
            idx,
            (unsigned)(uintptr_t)verify->fb,
            (unsigned)(uintptr_t)verify->rp,
            (unsigned)(uintptr_t)verify->mv_rp,
            (unsigned)verify->has_mv);
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
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;

    for (uint32_t i = 0; i < sd->fb_track_count; i++) {
        if (sd->fb_tracks[i].fb == framebuffer) {
            uint32_t last = --sd->fb_track_count;

            sd->fb_tracks[i] = sd->fb_tracks[last];

            break;
        }
    }

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
    extern StereoDevice g_devices[];
    extern uint32_t     g_device_count;
    StereoDevice *sd   = NULL;
    VkRenderPass mv_rp = VK_NULL_HANDLE;
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
            bool rp_match =
                (
                    dev->fb_tracks[i].rp != VK_NULL_HANDLE &&
                    pRenderPassBegin->renderPass != VK_NULL_HANDLE &&
                    dev->fb_tracks[i].rp == pRenderPassBegin->renderPass
                )
                ||
                (
                    dev->fb_tracks[i].mv_rp != VK_NULL_HANDLE &&
                    dev->fb_tracks[i].mv_rp == pRenderPassBegin->renderPass
                );
            if (fb_match) {
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
            }
            if (dev->fb_tracks[i].fb == pRenderPassBegin->framebuffer)
            {
                STEREO_LOG(
                    "FB_MATCH requested=%p fb_original=%p fb_used=%p fb_mv=%p",
                    pRenderPassBegin->renderPass,
                    dev->fb_tracks[i].rp,
                    dev->fb_tracks[i].rp_used_at_create,
                    dev->fb_tracks[i].mv_rp);
                if (dev->fb_tracks[i].has_mv)
                {
                    VkRenderPass candidate = VK_NULL_HANDLE;
                    StereoRenderPassInfo *rpi =
                        stereo_rp_lookup(dev,
                                         pRenderPassBegin->renderPass);
                    if (rpi && rpi->mv_handle)
                    {
                        candidate = rpi->mv_handle;
                        STEREO_LOG(
                            "RP_LOOKUP_SELECTED requested=%p original=%p mv=%p",
                            (void*)pRenderPassBegin->renderPass,
                            (void*)rpi->handle,
                            (void*)rpi->mv_handle);
                    }
                    else
                    {
                        STEREO_LOG(
                            "RP_LOOKUP_FAILED requested=%p fb=%p tracked_original=%p tracked_used=%p tracked_mv=%p",
                            (void*)pRenderPassBegin->renderPass,
                            (void*)pRenderPassBegin->framebuffer,
                            (void*)dev->fb_tracks[i].rp,
                            (void*)dev->fb_tracks[i].rp_used_at_create,
                            (void*)dev->fb_tracks[i].mv_rp);
                    }
                    if (candidate != VK_NULL_HANDLE)
                    {
                    STEREO_LOG(
                        "FB_SELECT fb=%p requested=%p tracked=%p tracked_used=%p tracked_mv=%p has_mv=%u",
                        (void*)pRenderPassBegin->framebuffer,
                        (void*)pRenderPassBegin->renderPass,
                        (void*)dev->fb_tracks[i].rp,
                        (void*)dev->fb_tracks[i].rp_used_at_create,
                        (void*)dev->fb_tracks[i].mv_rp,
                        (unsigned)dev->fb_tracks[i].has_mv);
                    STEREO_LOG(
                        "MV_SELECT fb=%p candidate=%p",
                        pRenderPassBegin->framebuffer,
                        candidate);
                    mv_rp = candidate;
                    STEREO_LOG(
                        "MV_SELECTED mv_rp=%p",
                        mv_rp);
                    sd = dev;
                    break;
                    }
                }

                STEREO_LOG(
                    "FB_MATCH_RESOLVE fb=%p rp_begin=%p tracked_rp=%p mv_rp=%p has_mv=%u rp_match=%u",
                    pRenderPassBegin->framebuffer,
                    pRenderPassBegin->renderPass,
                    dev->fb_tracks[i].rp,
                    dev->fb_tracks[i].mv_rp,
                    dev->fb_tracks[i].has_mv,
                    rp_match);
                break;
            }
        }
    }
    STEREO_LOG(
        "MV_AFTER_SCAN sd=%p mv_rp=%p",
        sd,
        mv_rp);
    if (!sd) {
        /* Framebuffer not in our tracking → non-MV; find any live device */
        for (uint32_t d = 0; d < g_device_count; d++) {
            if (g_devices[d].real_device) { sd = &g_devices[d]; break; }
        }
    }
    if (!sd) return;
    StereoRenderPassInfo *lookup =
        stereo_rp_lookup(sd, pRenderPassBegin->renderPass);
    
    STEREO_LOG(
        "RP_LOOKUP_BEGIN requested=%p lookup=%p lookup_orig=%p lookup_mv=%p",
        (void*)pRenderPassBegin->renderPass,
        (void*)lookup,
        lookup ? (void*)lookup->handle : NULL,
        lookup ? (void*)lookup->mv_handle : NULL);
    /* CRITICAL DIAGNOSTIC: MV expected but not resolved */
    if (mv_rp == VK_NULL_HANDLE)
    {
        STEREO_LOG(
            "MV RP LOST BEFORE DRAW CALL fb=%p rp=%p (this frame will be mono/black if expected stereo)",
            pRenderPassBegin->framebuffer,
            pRenderPassBegin->renderPass);
    }

    STEREO_LOG(
        "RP_BEGIN fb=%p mv_rp=%p active=%d",
        pRenderPassBegin->framebuffer,
        mv_rp,
        mv_rp != VK_NULL_HANDLE);
    if (mv_rp) {
        VkRenderPassBeginInfo modified = *pRenderPassBegin;
        modified.renderPass = mv_rp;
        STEREO_LOG(
            "BEGIN_PASS_DRIVER original=%p mv=%p framebuffer=%p",
            (void*)pRenderPassBegin->renderPass,
            (void*)modified.renderPass,
            (void*)modified.framebuffer);
        STEREO_LOG(
            "RP_BEGIN_DRIVER rp=%p fb=%p",
            (void*)modified.renderPass,
            (void*)modified.framebuffer);
        STEREO_LOG(
            "[RP BEGIN MV] fb=%p rp=%p mv_rp=%p",
            pRenderPassBegin->framebuffer,
            pRenderPassBegin->renderPass,
            mv_rp);
        STEREO_LOG(
            "CB_DISPATCH cb=%p sd=%p real_dev=%p",
            commandBuffer,
            sd,
            sd->real_device);
        sd->real.CmdBeginRenderPass(commandBuffer, &modified, contents);
        STEREO_LOG(
            "[RP BEGIN MONO] fb=%p rp=%p",
            pRenderPassBegin->framebuffer,
            pRenderPassBegin->renderPass);
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
            commandBuffer,
            sd,
            sd->real_device);
        sd->real.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }
}

VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline)
{
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

    if (!sd)
        return;

    STEREO_LOG(
        "PIPE_BIND cb=%p pipeline=%p bindPoint=%u cmd=%p",
        (void*)commandBuffer,
        (void*)pipeline,
        bindPoint,
        (void*)commandBuffer);
    sd->real.CmdBindPipeline(
        commandBuffer,
        pipelineBindPoint,
        pipeline);
}