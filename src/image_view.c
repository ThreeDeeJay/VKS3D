/*
 * image_view.c -- vkCreateImageView intercept for stereo multiview
 *
 * PROBLEM:
 *   VKS3D returns stereo_images[] (VkImage, arrayLayers=2) to the app via
 *   GetSwapchainImagesKHR.  The app then creates image views for its
 *   framebuffers using the standard swapchain pattern:
 *     viewType  = VK_IMAGE_VIEW_TYPE_2D
 *     layerCount = 1
 *
 *   However, multiview render passes (viewMask=0x3) require framebuffer
 *   attachments to be 2D_ARRAY views with layerCount >= 2.  A 2D view
 *   covering only layer 0 causes VK_ERROR_DEVICE_LOST in the app's own
 *   render submit -- before VKS3D even reaches the composite blit.
 *
 * FIX:
 *   Intercept vkCreateImageView.  When the image being viewed is one of
 *   our stereo render targets (a 2-layer array image we allocated in
 *   alloc_stereo_image), silently upgrade:
 *     viewType   2D        -> VK_IMAGE_VIEW_TYPE_2D_ARRAY
 *     layerCount 1         -> 2  (covers both eyes)
 *   All other images pass through unchanged.
 *
 * NOTE ON FRAMEBUFFER LAYERS:
 *   The Vulkan spec (VK_KHR_multiview) requires framebuffer.layers == 1
 *   when multiview is active.  Apps already set this for swapchain
 *   framebuffers, so no vkCreateFramebuffer intercept is needed.
 */

#include <string.h>
#include "stereo_icd.h"

/* ---- helper: is this VkImage one of our stereo render targets? --------- */
/*
 * Returns the swapchain that owns 'image', or NULL if it is not a stereo
 * render target.  Caller holds no lock (read-only scan of immutable arrays).
 */
static StereoSwapchain *find_owning_swapchain(StereoDevice *sd, VkImage image)
{
    for (uint32_t si = 0; si < sd->swapchain_count; si++) {
        StereoSwapchain *sc = &sd->swapchains[si];
        if (!sc->stereo_active || !sc->stereo_images) continue;
        for (uint32_t ii = 0; ii < sc->image_count; ii++) {
            if (sc->stereo_images[ii] == image)
                return sc;
        }
    }
    return NULL;
}

/* ---- vkCreateImageView ------------------------------------------------- */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImageView(
    VkDevice                        device,
    const VkImageViewCreateInfo    *pCreateInfo,
    const VkAllocationCallbacks    *pAllocator,
    VkImageView                    *pView)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* Passthrough for non-stereo devices or disabled stereo */
    if (!sd->stereo.enabled) {
        return sd->real.CreateImageView(sd->real_device, pCreateInfo,
                                        pAllocator, pView);
    }

    /* Is this image one of our stereo render targets? */
    StereoSwapchain *sc = find_owning_swapchain(sd, pCreateInfo->image);

    if (!sc) {
        /* Not a stereo image -- pass through unchanged */
        return sd->real.CreateImageView(sd->real_device, pCreateInfo,
                                        pAllocator, pView);
    }

    /*
     * This is a stereo render target (2-layer array image).
     * The app is creating a view for its framebuffer attachment.
     * Upgrade to VK_IMAGE_VIEW_TYPE_2D_ARRAY covering both layers so
     * the multiview render pass (viewMask=0x3) can write to both eyes.
     *
     * We accept whatever the app requested for baseArrayLayer (usually 0)
     * and simply set layerCount=2 and viewType=2D_ARRAY.
     */
    VkImageViewCreateInfo upgraded = *pCreateInfo;

    if (upgraded.viewType == VK_IMAGE_VIEW_TYPE_2D) {
        upgraded.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        STEREO_LOG("CreateImageView: upgrading stereo image %p view: "
                   "2D/layerCount=%u -> 2D_ARRAY/layerCount=2",
                   (void*)pCreateInfo->image,
                   pCreateInfo->subresourceRange.layerCount);
    }

    /* Always ensure layerCount covers both eyes */
    if (upgraded.subresourceRange.layerCount < 2)
        upgraded.subresourceRange.layerCount = 2;

    return sd->real.CreateImageView(sd->real_device, &upgraded,
                                    pAllocator, pView);
}
