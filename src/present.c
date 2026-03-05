/*
 * present.c — Side-By-Side composite blit pass
 *
 * Called at vkQueuePresentKHR time, before the real present.
 *
 * We blit:
 *   - stereo_images[idx] layer 0  →  sbs_images[idx] left  half  [0, W)
 *   - stereo_images[idx] layer 1  →  sbs_images[idx] right half  [W, 2W)
 *
 * This uses vkCmdBlitImage inside a one-time command buffer allocated
 * from a persistent command pool per swapchain.
 *
 * Memory barriers correctly transition image layouts before/after the blit.
 *
 * At the end we submit the composite command buffer and WAIT for it before
 * returning to the caller.  A production implementation would pipeline this
 * with semaphores instead of blocking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* ── Ensure composite cmd pool exists ───────────────────────────────────── */
static VkResult ensure_composite_resources(StereoDevice *sd, StereoSwapchain *sc)
{
    if (sc->composite_cmd_pool != VK_NULL_HANDLE)
        return VK_SUCCESS;

    /* ── Command pool ────────────────────────────────────────────── */
    /* Find a queue family with graphics or transfer capability */
    uint32_t qf_count = 0;
    sd->si->real.GetPhysicalDeviceQueueFamilyProperties(
        sd->real_physdev, &qf_count, NULL);

    VkQueueFamilyProperties *qfps =
        malloc(qf_count * sizeof(VkQueueFamilyProperties));
    if (!qfps) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sd->si->real.GetPhysicalDeviceQueueFamilyProperties(
        sd->real_physdev, &qf_count, qfps);

    uint32_t qf_idx = 0;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qf_idx = i;
            break;
        }
    }
    free(qfps);

    VkCommandPoolCreateInfo cpci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qf_idx,
    };
    VkResult res = sd->real.CreateCommandPool(
        sd->real_device, &cpci, NULL, &sc->composite_cmd_pool);
    if (res != VK_SUCCESS) return res;

    /* ── Allocate one command buffer per swapchain image ──────────── */
    VkCommandBufferAllocateInfo cbai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = sc->composite_cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = sc->image_count,
    };
    res = sd->real.AllocateCommandBuffers(
        sd->real_device, &cbai, sc->composite_cmds);
    if (res != VK_SUCCESS) return res;

    STEREO_LOG("Composite resources created for swapchain %p", (void*)sc->real_swapchain);
    return VK_SUCCESS;
}

/* ── Image memory barrier helper ─────────────────────────────────────────── */
static void cmd_barrier(
    StereoDevice            *sd,
    VkCommandBuffer          cmd,
    VkImage                  image,
    uint32_t                 old_layout,   /* VkImageLayout */
    uint32_t                 new_layout,   /* VkImageLayout */
    VkAccessFlags            src_access,
    VkAccessFlags            dst_access,
    VkPipelineStageFlags     src_stage,
    VkPipelineStageFlags     dst_stage,
    uint32_t                 base_layer,
    uint32_t                 layer_count)
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

/* ── Record SBS blit command buffer for one swapchain image ──────────────── */
static VkResult record_composite_cmd(
    StereoDevice   *sd,
    StereoSwapchain *sc,
    uint32_t        idx)
{
    VkCommandBuffer cmd = sc->composite_cmds[idx];
    VkImage         src = sc->stereo_images[idx];   /* 2-layer W×H         */
    VkImage         dst = sc->sbs_images[idx];      /* 1-layer (2W)×H      */

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = sd->real.BeginCommandBuffer(cmd, &begin);
    if (res != VK_SUCCESS) return res;

    /* ── Barrier: stereo images color_attachment → transfer_src ──── */
    cmd_barrier(sd, cmd, src,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 2);   /* both layers */

    /* ── Barrier: SBS swapchain image undefined → transfer_dst ───── */
    cmd_barrier(sd, cmd, dst,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1);

    uint32_t W = sc->app_width;
    uint32_t H = sc->app_height;

    /* ── Blit layer 0 (left eye) → left half of SBS image ─────────── */
    {
        VkImageBlit blit = {
            .srcSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,   /* left eye */
                .layerCount     = 1,
            },
            .srcOffsets = { {0, 0, 0}, {(int32_t)W, (int32_t)H, 1} },
            .dstSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            /* Left half of the SBS image */
            .dstOffsets = { {0, 0, 0}, {(int32_t)W, (int32_t)H, 1} },
        };
        sd->real.CmdBlitImage(cmd,
            src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_NEAREST);
    }

    /* ── Blit layer 1 (right eye) → right half of SBS image ────────── */
    {
        VkImageBlit blit = {
            .srcSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 1,   /* right eye */
                .layerCount     = 1,
            },
            .srcOffsets = { {0, 0, 0}, {(int32_t)W, (int32_t)H, 1} },
            .dstSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            /* Right half of the SBS image: x offset by W */
            .dstOffsets = { {(int32_t)W, 0, 0}, {(int32_t)(W*2), (int32_t)H, 1} },
        };
        sd->real.CmdBlitImage(cmd,
            src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_NEAREST);
    }

    /* ── Barrier: SBS image transfer_dst → present_src ─────────────── */
    cmd_barrier(sd, cmd, dst,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 1);

    /* ── Barrier: stereo images transfer_src → color_attachment ──── */
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

/* ── Public: run composite pass and wait ─────────────────────────────────── */
/*
 * wait_sems / wait_stages: the app's pWaitSemaphores from QueuePresentKHR.
 *   We must pass these to the composite submit so the GPU waits for the
 *   app's rendering to finish before blitting the stereo images.
 *   Without this, the composite races the app's render commands → device lost.
 *
 * signal_sem (output): a semaphore we create and signal when composite finishes.
 *   The caller passes this to the real QueuePresentKHR as its wait semaphore,
 *   ensuring the display engine doesn't scan out before composite is done.
 *   Caller owns it and must destroy it after the real present returns.
 */
VkResult stereo_composite_to_sbs(
    StereoDevice    *sd,
    VkQueue          queue,
    StereoSwapchain *sc,
    uint32_t         image_index,
    uint32_t         wait_sem_count,
    const VkSemaphore *wait_sems,
    VkSemaphore     *out_signal_sem)
{
    *out_signal_sem = VK_NULL_HANDLE;

    VkResult res = ensure_composite_resources(sd, sc);
    if (res != VK_SUCCESS) return res;

    /* Re-record each frame (images change layout every frame) */
    res = sd->real.ResetCommandBuffer(sc->composite_cmds[image_index], 0);
    if (res != VK_SUCCESS) return res;

    res = record_composite_cmd(sd, sc, image_index);
    if (res != VK_SUCCESS) {
        STEREO_ERR("record_composite_cmd failed: %d", res);
        return res;
    }

    /* Create the completion semaphore the caller will pass to the real present */
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore done_sem = VK_NULL_HANDLE;
    res = sd->real.CreateSemaphore(sd->real_device, &sci, NULL, &done_sem);
    if (res != VK_SUCCESS) return res;

    /* Each wait semaphore needs a stage mask — we wait at TRANSFER for the blit */
    VkPipelineStageFlags *stage_masks = NULL;
    if (wait_sem_count > 0) {
        stage_masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
        if (!stage_masks) {
            sd->real.DestroySemaphore(sd->real_device, done_sem, NULL);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < wait_sem_count; i++)
            stage_masks[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    /* Submit composite: wait for app rendering, signal done_sem when finished */
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = wait_sem_count,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = stage_masks,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &sc->composite_cmds[image_index],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &done_sem,
    };

    /* Create a fence so we can CPU-wait for the composite to finish */
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    res = sd->real.CreateFence(sd->real_device, &fci, NULL, &fence);
    if (res != VK_SUCCESS) {
        free(stage_masks);
        sd->real.DestroySemaphore(sd->real_device, done_sem, NULL);
        return res;
    }

    res = sd->real.QueueSubmit(queue, 1, &submit, fence);
    if (res == VK_SUCCESS) {
        res = sd->real.WaitForFences(
            sd->real_device, 1, &fence, VK_TRUE, UINT64_MAX);
    }

    free(stage_masks);
    sd->real.DestroyFence(sd->real_device, fence, NULL);

    if (res != VK_SUCCESS) {
        STEREO_ERR("Composite submit/wait failed: %d", res);
        sd->real.DestroySemaphore(sd->real_device, done_sem, NULL);
        return res;
    }

    *out_signal_sem = done_sem;
    return VK_SUCCESS;
}
