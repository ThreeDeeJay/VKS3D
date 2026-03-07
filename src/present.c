/*
 * present.c — DXGI 1.2 stereo composite + present
 *
 * Called from swapchain.c::stereo_QueuePresentKHR for each frame.
 *
 * Flow:
 *   1. Record a staging command buffer:
 *        a. Transition stereo_images[idx]: COLOR_ATTACHMENT → TRANSFER_SRC
 *        b. vkCmdCopyImageToBuffer — layer 0 → buf[0 .. W*H*4)
 *                                     layer 1 → buf[W*H*4 .. 2*W*H*4)
 *        c. Transition back: TRANSFER_SRC → COLOR_ATTACHMENT
 *   2. Submit staging CB: wait on app's render-complete semaphores,
 *      signal stage_fences[idx] when done.
 *   3. CPU WaitForFences(stage_fences[idx]) — blocks until pixels are in
 *      stage_mapped[idx] (host-coherent, no explicit flush needed).
 *   4. dxgi_present_frame() uploads left/right halves to D3D11 textures
 *      and calls IDXGISwapChain::Present(1, 0).
 *
 * NOTE: stage_fences[idx] is reset here (before submit) and was waited on
 *       by AcquireNextImageKHR, so no double-wait hazard exists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"

/* ── Image memory barrier helper ─────────────────────────────────────────── */
static void cmd_barrier(
    StereoDevice        *sd,
    VkCommandBuffer      cmd,
    VkImage              image,
    VkImageLayout        old_layout,
    VkImageLayout        new_layout,
    VkAccessFlags        src_access,
    VkAccessFlags        dst_access,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage,
    uint32_t             base_layer,
    uint32_t             layer_count)
{
    VkImageMemoryBarrier imb = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = src_access,
        .dstAccessMask       = dst_access,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount   = 1,
            .baseArrayLayer = base_layer,
            .layerCount     = layer_count,
        },
    };
    sd->real.CmdPipelineBarrier(cmd,
        src_stage, dst_stage,
        0, 0, NULL, 0, NULL, 1, &imb);
}

/* ── Record staging command buffer for one swapchain image ──────────────── */
static VkResult record_staging_cmd(
    StereoDevice    *sd,
    StereoSwapchain *sc,
    uint32_t         idx)
{
    VkCommandBuffer  cmd = sc->stage_cmds[idx];
    VkImage          src = sc->stereo_images[idx];
    VkBuffer         dst = sc->stage_buf[idx];
    VkDeviceSize eye_sz  = (VkDeviceSize)sc->app_width * sc->app_height * 4;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = sd->real.BeginCommandBuffer(cmd, &begin);
    if (res != VK_SUCCESS) return res;

    /* ── Transition both layers: COLOR_ATTACHMENT → TRANSFER_SRC ──── */
    cmd_barrier(sd, cmd, src,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 2);

    /* ── Copy layer 0 (left eye) → buf[0 .. eye_sz) ─────────────────── */
    VkBufferImageCopy copy0 = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,   /* tightly packed */
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,  /* left eye */
            .layerCount     = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {sc->app_width, sc->app_height, 1},
    };
    sd->real.CmdCopyImageToBuffer(cmd, src,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &copy0);

    /* ── Copy layer 1 (right eye) → buf[eye_sz .. 2*eye_sz) ─────────── */
    VkBufferImageCopy copy1 = copy0;
    copy1.bufferOffset              = eye_sz;
    copy1.imageSubresource.baseArrayLayer = 1;  /* right eye */
    sd->real.CmdCopyImageToBuffer(cmd, src,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &copy1);

    /* ── Transition both layers back: TRANSFER_SRC → COLOR_ATTACHMENT ─ */
    cmd_barrier(sd, cmd, src,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 2);

    return sd->real.EndCommandBuffer(cmd);
}

/* ── Public: run DXGI stereo present ─────────────────────────────────────── */
VkResult stereo_dxgi_present(
    StereoDevice       *sd,
    VkQueue             queue,
    StereoSwapchain    *sc,
    uint32_t            image_index,
    uint32_t            wait_sem_count,
    const VkSemaphore  *wait_sems)
{
    if (!sc->stereo_active || !sc->dxgi_mode)
        return VK_SUCCESS;

    /* ── Reset and record staging command buffer ──────────────────── */
    VkResult res = sd->real.ResetCommandBuffer(sc->stage_cmds[image_index], 0);
    if (res != VK_SUCCESS) return res;

    res = record_staging_cmd(sd, sc, image_index);
    if (res != VK_SUCCESS) {
        STEREO_ERR("record_staging_cmd failed: %d", res);
        return res;
    }

    /* ── Build wait-stage mask array ─────────────────────────────── */
    VkPipelineStageFlags *stage_masks = NULL;
    if (wait_sem_count > 0) {
        stage_masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
        if (!stage_masks) return VK_ERROR_OUT_OF_HOST_MEMORY;
        /* Wait at TRANSFER stage — we blit from the stereo images */
        for (uint32_t i = 0; i < wait_sem_count; i++)
            stage_masks[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    /* ── Reset the staging fence (must not be pending before submit) ── */
    res = sd->real.ResetFences(sd->real_device, 1, &sc->stage_fences[image_index]);
    if (res != VK_SUCCESS) { free(stage_masks); return res; }

    /* ── Submit staging: wait on app render sems, signal fence ──────── */
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = wait_sem_count,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = stage_masks,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &sc->stage_cmds[image_index],
    };
    res = sd->real.QueueSubmit(queue, 1, &submit, sc->stage_fences[image_index]);
    free(stage_masks);
    if (res != VK_SUCCESS) {
        STEREO_ERR("Staging submit failed: %d", res);
        return res;
    }

    /* ── CPU wait: block until GPU→CPU copy is done ──────────────── */
    res = sd->real.WaitForFences(
        sd->real_device, 1, &sc->stage_fences[image_index], VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS) {
        STEREO_ERR("Staging fence wait failed: %d", res);
        return res;
    }

    /* ── Hand off to DXGI (pixels are now in stage_mapped[image_index]) */
    VkDeviceSize eye_sz = (VkDeviceSize)sc->app_width * sc->app_height * 4;
    const void *left_pixels  = sc->stage_mapped[image_index];
    const void *right_pixels = (const uint8_t*)sc->stage_mapped[image_index] + eye_sz;

    return dxgi_present_frame(sd, sc, left_pixels, right_pixels);
}
