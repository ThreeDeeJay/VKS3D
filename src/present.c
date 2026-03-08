/*
 * present.c — DXGI 1.2 stereo composite + present
 *
 * Mono rendering path (no multiview):
 *   The app renders a single frame into stereo_images[idx] (W×H, single layer).
 *   We copy that frame to BOTH DXGI eye slices so 3D Vision receives valid
 *   content on both eyes.  Both eyes see the same image (no parallax).
 *
 * Flow per frame:
 *   1. Record staging CB:
 *        a. Transition stereo_images[idx]: COLOR_ATTACHMENT → TRANSFER_SRC
 *        b. vkCmdCopyImageToBuffer: layer 0 → stage_buf[idx] (W×H×4 bytes)
 *        c. Transition back: TRANSFER_SRC → COLOR_ATTACHMENT
 *   2. Submit: wait on app's render-complete semaphores, signal stage_fences[idx]
 *   3. CPU WaitForFences → pixels available in stage_mapped[idx]
 *   4. dxgi_present_frame(left=stage_mapped, right=stage_mapped)
 *      ↳ UpdateSubresource both eye textures with same pixel data
 *      ↳ CopySubresourceRegion → DXGI back-buffer slices 0 and 1
 *      ↳ IDXGISwapChain::Present(1, 0)
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
    VkPipelineStageFlags dst_stage)
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
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount   = 1,
            .baseArrayLayer = 0, .layerCount   = 1,  /* single-layer mono image */
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
    VkCommandBuffer cmd = sc->stage_cmds[idx];
    VkImage         src = sc->stereo_images[idx];
    VkBuffer        dst = sc->stage_buf[idx];

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = sd->real.BeginCommandBuffer(cmd, &begin);
    if (res != VK_SUCCESS) return res;

    /* Transition: COLOR_ATTACHMENT → TRANSFER_SRC */
    cmd_barrier(sd, cmd, src,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* Copy single layer → host buffer */
    VkBufferImageCopy copy = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {sc->app_width, sc->app_height, 1},
    };
    sd->real.CmdCopyImageToBuffer(cmd, src,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &copy);

    /* Transition back: TRANSFER_SRC → COLOR_ATTACHMENT */
    cmd_barrier(sd, cmd, src,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

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

    /* Reset and record staging command buffer */
    VkResult res = sd->real.ResetCommandBuffer(sc->stage_cmds[image_index], 0);
    if (res != VK_SUCCESS) return res;

    res = record_staging_cmd(sd, sc, image_index);
    if (res != VK_SUCCESS) {
        STEREO_ERR("record_staging_cmd failed: %d", res);
        return res;
    }

    /* Build wait-stage mask array */
    VkPipelineStageFlags *stage_masks = NULL;
    if (wait_sem_count > 0) {
        stage_masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
        if (!stage_masks) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < wait_sem_count; i++)
            stage_masks[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    /* Reset fence before submit */
    res = sd->real.ResetFences(sd->real_device, 1, &sc->stage_fences[image_index]);
    if (res != VK_SUCCESS) { free(stage_masks); return res; }

    /* Submit staging: wait on app render sems, signal fence */
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

    /* CPU wait for GPU→CPU copy */
    res = sd->real.WaitForFences(
        sd->real_device, 1, &sc->stage_fences[image_index], VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS) {
        STEREO_ERR("Staging fence wait failed: %d", res);
        return res;
    }

    /* Same mono frame → both DXGI eyes (left=right=stage_mapped) */
    const void *pixels = sc->stage_mapped[image_index];
    return dxgi_present_frame(sd, sc, pixels, pixels);
}
