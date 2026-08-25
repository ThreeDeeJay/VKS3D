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
        STEREO_LOG(
            "FB_ATTACH_SCAN result all=%u attachmentCount=%u upgraded_views=%u rp=%p",
            (unsigned)all,
            pCreateInfo->attachmentCount,
            sd->upgraded_view_count,
            (void*)pCreateInfo->renderPass);
        /*
         * Resolve MV render pass independently of attachment scan.
         * Attachment upgrade status is diagnostic only; it should not
         * prevent using the MV render pass when a valid clone exists.
         */
        StereoRenderPassInfo *rpi =
            stereo_rp_lookup(sd, pCreateInfo->renderPass);
        STEREO_LOG(
            "FB_RP_RESOLVE request=%p rpi=%p handle=%p mv=%p has_mv=%u all=%u",
            (void*)pCreateInfo->renderPass,
            (void*)rpi,
            rpi ? (void*)rpi->handle : NULL,
            rpi ? (void*)rpi->mv_handle : NULL,
            rpi ? (unsigned)rpi->has_multiview : 0,
            (unsigned)all);
        if (rpi &&
            rpi->mv_handle &&
            (rpi->handle == pCreateInfo->renderPass))
        {
            fci.renderPass = rpi->mv_handle;
            use_mv = rpi->mv_handle;
            STEREO_LOG(
                "FB_SET renderPass=%p all=%u attachments=%u",
                fci.renderPass,
                (unsigned)all,
                pCreateInfo->attachmentCount);
        }
        else
        {
            STEREO_LOG(
                "FB_MV_NOT_SELECTED all=%u rpi=%p",
                (unsigned)all,
                (void*)rpi);
        }
        if (!all) {
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

         /* Reserve a unique tracking slot immediately.
          * This avoids two concurrent CreateFramebuffer calls both
          * writing the same entry before fb_track_count is advanced.
          */
        CHECK_ARRAY_COUNT(sd->fb_track_count, MAX_FB_TRACK, "fb_track_count");
         uint32_t idx = sd->fb_track_count++;
         STEREO_LOG(
             "FB_COUNT_RESERVE idx=%u next=%u",
             idx,
             sd->fb_track_count);
        if (idx >= MAX_FB_TRACK)
        {
            STEREO_LOG(
                "[FB OVERFLOW] idx=%u max=%u",
                idx,
                MAX_FB_TRACK);
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
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
            STEREO_LOG("[FB INFO] multiview enabled but use_mv == NULL fb=%p rp=%p",
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
        StereoFramebufferTrack *verify =
            &sd->fb_tracks[idx];
        for (uint32_t j = 0; j < idx; j++)
        {
            if (sd->fb_tracks[j].fb == verify->fb)
            {
                STEREO_LOG(
                    "[FB DUPLICATE] idx=%u previous=%u fb=%p",
                    idx,
                    j,
                    verify->fb);
            }
        }
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
    STEREO_LOG("CALLED stereo_DestroyFramebuffer");
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;
    for (uint32_t i = 0; i < sd->fb_track_count; i++) {
        if (sd->fb_tracks[i].fb == framebuffer) {
            uint32_t last = --sd->fb_track_count;
            if (i != last)
                sd->fb_tracks[i] = sd->fb_tracks[last];
            memset(&sd->fb_tracks[last], 0, sizeof(sd->fb_tracks[last]));
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
    STEREO_LOG("CALLED stereo_CmdBeginRenderPass");
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
                    dev->fb_tracks[i].rp &&
                    pRenderPassBegin->renderPass &&
                    dev->fb_tracks[i].rp == pRenderPassBegin->renderPass
                )
                ||
                (
                    dev->fb_tracks[i].mv_rp &&
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
                    "FB_TRACK_MATCH fb=%p tracked_rp=%p tracked_used=%p tracked_mv=%p has_mv=%u",
                    (void*)dev->fb_tracks[i].fb,
                    (void*)dev->fb_tracks[i].rp,
                    (void*)dev->fb_tracks[i].rp_used_at_create,
                    (void*)dev->fb_tracks[i].mv_rp,
                    (unsigned)dev->fb_tracks[i].has_mv);
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
                        STEREO_LOG(
                            "RP_LOOKUP request=%p result=%p",
                            (void*)pRenderPassBegin->renderPass,
                            (void*)rpi);
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
                /* lookup failed, keep searching */
                continue;
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
        "BEGIN_RP cb=%p rp=%p mv=%u mv_handle=%p",
        (void *)commandBuffer,
        (void *)(mv_rp ? mv_rp : pRenderPassBegin->renderPass),
        lookup ? lookup->has_multiview : 0,
        lookup ? (void *)lookup->mv_handle : NULL);
    remember_begin_renderpass(
        sd,
        commandBuffer,
        mv_rp ? mv_rp : pRenderPassBegin->renderPass,
        0);
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
            pRenderPassBegin->framebuffer,
            pRenderPassBegin->renderPass,
            mv_rp);
        STEREO_LOG(
            "CB_DISPATCH cb=%p sd=%p real_dev=%p",
            commandBuffer,
            sd,
            sd->real_device);
        for (uint32_t i = 0; i < sd->cb_track_count; i++)
        {
            if (sd->cb_track[i].cb == commandBuffer)
            {
                sd->cb_track[i].render_pass = modified.renderPass;
                sd->cb_track[i].framebuffer = modified.framebuffer;
                break;
            }
        }
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
    if (sd->stereo.multiview && modified.viewMask == 0)
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
            "DRAW_INDEXED pipe=%p rp=%p fb=%p quad=%u patched_vs=%u patched_fs=%u",
            (void *)pipe,
            (void *)rp,
            (void *)fb,
            info->is_quad,
            info->patched_vs,
            info->patched_fs);
    }
    else
    {
        STEREO_LOG(
            "DRAW_INDEXED pipe=%p UNKNOWN",
            (void *)pipe);
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
        "DRAW_STEREO cmd=%p active=%u viewMask=0x%x",
        (void*)commandBuffer,
        sd && sd->stereo.active_rendering ? 1u : 0u,
        sd ? sd->stereo.active_view_mask : 0u);
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
            "SHADER_OBJECT_BIND i=%u stage=0x%x shader=%p",
            i,
            pStages ? pStages[i] : 0,
            pShaders ? (void*)(uintptr_t)pShaders[i] : NULL);
    }
    sd->real.CmdBindShadersEXT(
        commandBuffer,
        stageCount,
        pStages,
        pShaders);
}