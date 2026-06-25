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
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    VkFramebufferCreateInfo fci = *pCreateInfo;
    /* CRITICAL: snapshot ORIGINAL RP before any modification */
    VkRenderPass original_rp = pCreateInfo->renderPass;
    VkRenderPass use_mv      = VK_NULL_HANDLE;

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
    VkResult res = sd->real.CreateFramebuffer(sd->real_device, &fci, pAllocator, pFramebuffer);
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
        uint32_t idx = sd->fb_track_count;

        VkRenderPass real_rp = original_rp;

        /* IMPORTANT: snapshot BEFORE increment */
        StereoFramebufferTrack *t = &sd->fb_tracks[idx];
        memset(t, 0, sizeof(*t));

        t->fb     = *pFramebuffer;
        t->rp     = fci.renderPass;   // IMPORTANT: actual used RP
        t->mv_rp  = use_mv;
        if (use_mv != VK_NULL_HANDLE && !sd->stereo.multiview)
        {
            STEREO_LOG("[HARD ASSERT] MV RP exists but multiview disabled?? fb=%p rp=%p",
                       t->fb, t->rp);
        }
        
        if (use_mv != VK_NULL_HANDLE && sd->multiview_pass_exists == false)
        {
            STEREO_LOG("[HARD ASSERT] MV RP created but global flag not set fb=%p", t->fb);
        }
        t->has_mv = (use_mv != VK_NULL_HANDLE) &&
                    sd->stereo.multiview &&
                    (use_mv == fci.renderPass || use_mv == pCreateInfo->renderPass);

        STEREO_LOG(
            "FB_TRACK_CREATE idx=%u fb=%p rp=%p mv_rp=%p has_mv=%u mv_enabled=%u",
            idx,
            t->fb,
            t->rp,
            t->mv_rp,
            t->has_mv,
            sd->stereo.multiview);
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
            "FB_TRACK_VERIFY idx=%u fb=%p rp=%p mv_rp=%p has_mv=%u",
            idx,
            verify->fb,
            verify->rp,
            verify->mv_rp,
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
    for (uint32_t d = 0; d < g_device_count && !sd; d++) {
        StereoDevice *dev = &g_devices[d];
        STEREO_LOG(
            "FB_TRACK_SCAN count=%u sizeof(track)=%u",
            dev->fb_track_count,
            (unsigned)sizeof(StereoFramebufferTrack));
        for (uint32_t i = 0; i < dev->fb_track_count; i++) {

            bool fb_match = (dev->fb_tracks[i].fb == pRenderPassBegin->framebuffer);
            bool rp_match =
                (dev->fb_tracks[i].rp == pRenderPassBegin->renderPass) ||
                (dev->fb_tracks[i].mv_rp != VK_NULL_HANDLE &&
                 dev->fb_tracks[i].mv_rp == pRenderPassBegin->renderPass);
            if (fb_match) {
                STEREO_LOG(
                    "FB_MATCH_CANDIDATE d=%u i=%u fb=%p rp_begin=%p tracked_rp=%p mv_rp=%p has_mv=%u rp_match=%u",
                    d,
                    i,
                    pRenderPassBegin->framebuffer,
                    pRenderPassBegin->renderPass,
                    dev->fb_tracks[i].rp,
                    dev->fb_tracks[i].mv_rp,
                    dev->fb_tracks[i].has_mv,
                    rp_match);
            }
            if (dev->fb_tracks[i].fb == pRenderPassBegin->framebuffer)
            {
                bool rp_match = (dev->fb_tracks[i].rp == pRenderPassBegin->renderPass);

                VkRenderPass resolved_mv = VK_NULL_HANDLE;

                if (resolved_mv && dev->fb_tracks[i].mv_rp == VK_NULL_HANDLE)
                {
                    dev->fb_tracks[i].mv_rp  = resolved_mv;
                    dev->fb_tracks[i].has_mv = true;
                
                    STEREO_LOG(
                        "[FB_MATCH_WRITEBACK] fb=%p rp=%p mv_rp=%p",
                        dev->fb_tracks[i].fb,
                        dev->fb_tracks[i].rp,
                        resolved_mv);
                }
                if (dev->fb_tracks[i].has_mv)
                {
                    VkRenderPass candidate = dev->fb_tracks[i].mv_rp;
                
                    if (candidate != VK_NULL_HANDLE &&
                        (rp_match || candidate == pRenderPassBegin->renderPass))
                    {
                        mv_rp = candidate;
                        sd = dev;
                        break;
                    }
                }
                if (resolved_mv && dev->fb_tracks[i].mv_rp == VK_NULL_HANDLE)
                {
                    dev->fb_tracks[i].mv_rp  = resolved_mv;
                    dev->fb_tracks[i].has_mv = true;

                    STEREO_LOG(
                        "[FB_MATCH_WRITEBACK] fb=%p mv_rp=%p",
                        dev->fb_tracks[i].fb,
                        resolved_mv);
                }

                STEREO_LOG(
                    "FB_MATCH_RESOLVE fb=%p rp_begin=%p tracked_rp=%p mv_rp=%p has_mv=%u rp_match=%u resolved_mv=%p",
                    pRenderPassBegin->framebuffer,
                    pRenderPassBegin->renderPass,
                    dev->fb_tracks[i].rp,
                    dev->fb_tracks[i].mv_rp,
                    dev->fb_tracks[i].has_mv,
                    rp_match,
                    resolved_mv);

                if (resolved_mv)
                {
                    mv_rp = resolved_mv;
                }

                break;
            }
        }
    }
    if (!sd) {
        /* Framebuffer not in our tracking → non-MV; find any live device */
        for (uint32_t d = 0; d < g_device_count; d++) {
            if (g_devices[d].real_device) { sd = &g_devices[d]; break; }
        }
    }
    if (!sd) return;
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
            "[RP BEGIN MV] fb=%p rp=%p mv_rp=%p",
            pRenderPassBegin->framebuffer,
            pRenderPassBegin->renderPass,
            mv_rp);
        sd->real.CmdBeginRenderPass(commandBuffer, &modified, contents);
        STEREO_LOG(
            "[RP BEGIN MONO] fb=%p rp=%p",
            pRenderPassBegin->framebuffer,
            pRenderPassBegin->renderPass);
    } else {
        sd->real.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }
}