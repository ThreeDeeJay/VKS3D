/*
 * present.c — DXGI stereo present (external-memory, no CPU staging)
 *
 * Flow per frame
 * ──────────────
 * 1. Record barrier CB:
 *      a. Transition stereo_images[0]: COLOR_ATTACHMENT → GENERAL
 *         (GENERAL is the cross-API handoff layout for external images)
 *      b. Covers BOTH layers (baseArrayLayer=0, layerCount=2)
 * 2. Submit: wait on app's render-complete semaphores, signal barrier_fence
 * 3. CPU WaitForFences — waits only for render completion, not PCIe transfer
 * 4. dxgi_copy_and_present:
 *      GPU CopySubresourceRegion shared_tex[0] → DXGI back-buf[0]
 *      GPU CopySubresourceRegion shared_tex[1] → DXGI back-buf[1]
 *      IDXGISwapChain::Present(1, 0)
 * 5. barrier_fence is reset here before the submit; AcquireNextImageKHR
 *    waits on it again before the next render — this is the backpressure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"

/* ── Image memory barrier helper ────────────────────────────────────────── */
static void cmd_image_barrier(
    StereoDevice        *sd,
    VkCommandBuffer      cmd,
    VkImage              image,
    uint32_t             layer_count,
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
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount   = 1,
            .baseArrayLayer = 0, .layerCount   = layer_count,
        },
    };
    sd->real.CmdPipelineBarrier(cmd,
        src_stage, dst_stage,
        0, 0, NULL, 0, NULL, 1, &imb);
}

/* ── Record the present-barrier command buffer ──────────────────────────── */
static VkResult record_barrier_cmd(
    StereoDevice    *sd,
    StereoSwapchain *sc)
{
    VkCommandBuffer cmd = sc->barrier_cmds[0];

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = sd->real.BeginCommandBuffer(cmd, &begin);
    if (res != VK_SUCCESS) return res;

    /* Transition: COLOR_ATTACHMENT_OPTIMAL → GENERAL (both eye layers).
     * GENERAL is the cross-API handoff layout for external images;
     * D3D11 will read the texture in its native tiled layout.         */
    cmd_image_barrier(sd, cmd, sc->stereo_images[0],
        2, /* both layers */
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    return sd->real.EndCommandBuffer(cmd);
}

/* ── Public: run DXGI stereo present (external memory path) ─────────────── */
VkResult stereo_dxgi_present(
    StereoDevice       *sd,
    VkQueue             queue,
    StereoSwapchain    *sc,
    uint32_t            image_index,    /* always 0 in external-mem mode */
    uint32_t            wait_sem_count,
    const VkSemaphore  *wait_sems)
{
    (void)image_index; /* single image — always index 0 */

    if (!sc->stereo_active || !sc->dxgi_mode)
        return VK_SUCCESS;

    /* Reset + record barrier CB */
    VkResult res = sd->real.ResetCommandBuffer(sc->barrier_cmds[0], 0);
    if (res != VK_SUCCESS) { STEREO_ERR("ResetCommandBuffer failed: %d", res); return res; }

    res = record_barrier_cmd(sd, sc);
    if (res != VK_SUCCESS) { STEREO_ERR("record_barrier_cmd failed: %d", res); return res; }

    /* Build wait-stage masks */
    VkPipelineStageFlags *stage_masks = NULL;
    if (wait_sem_count > 0) {
        stage_masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
        if (!stage_masks) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < wait_sem_count; i++)
            stage_masks[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }

    /* Reset fence before submit (fence starts SIGNALED from init / prev frame) */
    res = sd->real.ResetFences(sd->real_device, 1, &sc->barrier_fences[0]);
    if (res != VK_SUCCESS) { free(stage_masks); return res; }

    /* Submit barrier: wait for app's render sems, signal fence */
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = wait_sem_count,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = stage_masks,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &sc->barrier_cmds[0],
    };
    res = sd->real.QueueSubmit(queue, 1, &submit, sc->barrier_fences[0]);
    free(stage_masks);
    if (res != VK_SUCCESS) {
        STEREO_ERR("Barrier submit failed: %d", res);
        return res;
    }

    /* Wait for render + barrier to complete (usually < 1ms at high FPS;
     * this is just waiting for GPU render, NOT a PCIe data transfer)    */
    res = sd->real.WaitForFences(
        sd->real_device, 1, &sc->barrier_fences[0], VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS) {
        STEREO_ERR("Barrier fence wait failed: %d", res);
        return res;
    }

    /* GPU copy shared_tex → DXGI back buffer, then Present.
     * The shared texture is now in GENERAL layout and render is complete.
     * D3D11 reads the same physical GPU pages — no PCIe transfer.       */
    return dxgi_copy_and_present(sd, sc);
}
