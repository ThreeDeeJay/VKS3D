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

    VkFramebufferCreateInfo fci    = *pCreateInfo;
    VkRenderPass            use_mv = VK_NULL_HANDLE;

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
            if (rpi && rpi->mv_handle) {
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
        
                if (!found) {
                    STEREO_LOG(
                        "[FB NON-UPGRADED] att=%u view=%p",
                        i,
                        pCreateInfo->pAttachments[i]);
                }
            }
        }
    }

    VkResult res = sd->real.CreateFramebuffer(sd->real_device, &fci, pAllocator, pFramebuffer);
    if (res == VK_SUCCESS && use_mv && sd->fb_track_count < MAX_FB_TRACK) {
        sd->fb_track_handles[sd->fb_track_count] = *pFramebuffer;
        sd->fb_track_mv_rps [sd->fb_track_count] = use_mv;
        sd->fb_track_count++;
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
        if (sd->fb_track_handles[i] == framebuffer) {
            uint32_t last = --sd->fb_track_count;
            sd->fb_track_handles[i] = sd->fb_track_handles[last];
            sd->fb_track_mv_rps [i] = sd->fb_track_mv_rps [last];
            break;
        }
    }
    sd->real.DestroyFramebuffer(sd->real_device, framebuffer, pAllocator);
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
    VkRenderPass  mv_rp = VK_NULL_HANDLE;

    for (uint32_t d = 0; d < g_device_count && !sd; d++) {
        StereoDevice *dev = &g_devices[d];
        for (uint32_t i = 0; i < dev->fb_track_count; i++) {
            if (dev->fb_track_handles[i] == pRenderPassBegin->framebuffer) {
                sd    = dev;
                mv_rp = dev->fb_track_mv_rps[i];
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

    if (mv_rp) {
        VkRenderPassBeginInfo modified = *pRenderPassBegin;
        modified.renderPass = mv_rp;
        sd->real.CmdBeginRenderPass(commandBuffer, &modified, contents);
    } else {
        sd->real.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }
}