/*
 * render_pass.c — Render pass intercept
 *
 * For DXGI-mode stereo (3D Vision) we do NOT inject VK_KHR_multiview.
 *
 * Why not: multiview requires every framebuffer attachment — including depth/
 * stencil — to be an array image with arrayLayers >= 2.  We cannot safely
 * inject viewMask=0x3 without also intercepting vkCreateImage to produce
 * 2-layer depth buffers.  Doing so causes an invalid GPU access on depth
 * layer 1 → TDR → VK_ERROR_DEVICE_LOST on the first frame.
 *
 * Instead the stereo render target is a normal W×H single-layer image.
 * At present time the same mono frame is copied to both DXGI eye slices.
 * Both eyes see identical content and 3D Vision engages correctly.
 * Per-eye parallax offset requires depth interception and is a future step.
 *
 * What we DO patch:
 *   finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR  →  COLOR_ATTACHMENT_OPTIMAL
 *
 *   Our stereo render target is a plain VkImage, not a swapchain image.
 *   Transitioning it to PRESENT_SRC_KHR is undefined behaviour.  Patching
 *   to COLOR_ATTACHMENT_OPTIMAL leaves it in a valid layout for the
 *   subsequent vkCmdCopyImageToBuffer staging copy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* ── vkCreateRenderPass ─────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRenderPass(
    VkDevice                        device,
    const VkRenderPassCreateInfo   *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkRenderPass                   *pRenderPass)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* Detect swapchain pass: any attachment finalLayout == PRESENT_SRC_KHR */
    bool is_swapchain_pass = false;
    if (sd->stereo.enabled && pCreateInfo->attachmentCount > 0) {
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            if (pCreateInfo->pAttachments[i].finalLayout
                    == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                is_swapchain_pass = true;
                break;
            }
        }
    }

    if (!is_swapchain_pass) {
        /* Passthrough — offscreen pass or stereo disabled */
        VkResult res = sd->real.CreateRenderPass(
            sd->real_device, pCreateInfo, pAllocator, pRenderPass);
        if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
            StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
            rpi->handle        = *pRenderPass;
            rpi->has_multiview = false;
            rpi->view_mask     = 0;
            rpi->subpass_count = pCreateInfo->subpassCount;
        }
        return res;
    }

    /* Patch PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL on swapchain pass */
    VkAttachmentDescription *patched =
        malloc(pCreateInfo->attachmentCount * sizeof(VkAttachmentDescription));
    if (!patched) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        patched[i] = pCreateInfo->pAttachments[i];
        if (patched[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            patched[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderPassCreateInfo modified = *pCreateInfo;
    modified.pAttachments = patched;

    VkResult res = sd->real.CreateRenderPass(
        sd->real_device, &modified, pAllocator, pRenderPass);
    free(patched);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = false;
        rpi->view_mask     = 0;
        rpi->subpass_count = pCreateInfo->subpassCount;
        STEREO_LOG("RenderPass %p: finalLayout patched (PRESENT_SRC->COLOR_ATTACHMENT)",
                   (void*)*pRenderPass);
    }
    return res;
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
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* Detect swapchain pass */
    bool is_swapchain_pass = false;
    if (sd->stereo.enabled && pCreateInfo->attachmentCount > 0) {
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
            if (pCreateInfo->pAttachments[i].finalLayout
                    == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                is_swapchain_pass = true;
                break;
            }
        }
    }

    if (!sd->real.CreateRenderPass2KHR) {
        /* Driver doesn't support rp2 natively — shouldn't reach here since
         * we only inject VK_KHR_create_renderpass2 when the driver has it */
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    if (!is_swapchain_pass) {
        return sd->real.CreateRenderPass2KHR(
            sd->real_device, pCreateInfo, pAllocator, pRenderPass);
    }

    /* Patch PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL */
    VkAttachmentDescription2 *patched =
        malloc(pCreateInfo->attachmentCount * sizeof(VkAttachmentDescription2));
    if (!patched) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        patched[i] = pCreateInfo->pAttachments[i];
        if (patched[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            patched[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkRenderPassCreateInfo2 modified = *pCreateInfo;
    modified.pAttachments = patched;

    VkResult res = sd->real.CreateRenderPass2KHR(
        sd->real_device, &modified, pAllocator, pRenderPass);
    free(patched);

    if (res == VK_SUCCESS && sd->render_pass_count < MAX_RENDER_PASSES) {
        StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
        rpi->handle        = *pRenderPass;
        rpi->has_multiview = false;
        rpi->view_mask     = 0;
        rpi->subpass_count = pCreateInfo->subpassCount;
        STEREO_LOG("RenderPass2 %p: finalLayout patched (PRESENT_SRC->COLOR_ATTACHMENT)",
                   (void*)*pRenderPass);
    }
    return res;
}
#endif /* VK_KHR_create_renderpass2 */
