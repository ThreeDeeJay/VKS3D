#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "stereo_icd.h"
#include "NV3D.hpp"

/* ------------------------------------------------------------------------- */
/* Local helpers                                                             */
/* ------------------------------------------------------------------------- */

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
.baseMipLevel   = 0,
.levelCount     = 1,
.baseArrayLayer = 0,
.layerCount     = layer_count,
},
};

sd->real.CmdPipelineBarrier(
    cmd,
    src_stage,
    dst_stage,
    0,
    0, NULL,
    0, NULL,
    1, &imb);

}

/* ------------------------------------------------------------------------- */
/* NV3D resource creation                                                    */
/* ------------------------------------------------------------------------- */

bool nv3d_init(
StereoDevice *sd,
uint32_t width,
uint32_t height)
{
if (sd->nv3d_ok &&
sd->nv3d_width  == width &&
sd->nv3d_height == height)
{
return true;
}

nv3d_destroy(sd);

InterfaceVulkan *iface = CreateInterfaceVulkan();
if (!iface)
{
    STEREO_ERR("[NV3D] CreateInterfaceVulkan failed");
    return false;
}

HANDLE mem_handle   = NULL;
HANDLE fence_handle = NULL;

HRESULT hr =
    iface->InitSharedResources(
        width,
        height,
        87, /* DXGI_FORMAT_B8G8R8A8_UNORM */
        &mem_handle,
        &fence_handle);

if (FAILED(hr))
{
    STEREO_ERR(
        "[NV3D] InitSharedResources failed: 0x%08x",
        (unsigned)hr);

    iface->Delete();
    return false;
}

sd->nv3d_iface        = iface;
sd->nv3d_mem_handle   = mem_handle;
sd->nv3d_fence_handle = fence_handle;
sd->nv3d_width        = width;
sd->nv3d_height       = height;
sd->nv3d_value        = 0;

/* ------------------------------------------------------------- */
/* Imported image                                                */
/* ------------------------------------------------------------- */

VkExternalMemoryImageCreateInfo ext_img = {
    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    .handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
};

VkImageCreateInfo ici = {
    .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .pNext         = &ext_img,
    .imageType     = VK_IMAGE_TYPE_2D,
    .format        = VK_FORMAT_B8G8R8A8_UNORM,
    .extent        = { width, height, 1 },
    .mipLevels     = 1,
    .arrayLayers   = 2,
    .samples       = VK_SAMPLE_COUNT_1_BIT,
    .tiling        = VK_IMAGE_TILING_OPTIMAL,
    .usage         =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
};

if (sd->real.CreateImage(
        sd->real_device,
        &ici,
        NULL,
        &sd->nv3d_image) != VK_SUCCESS)
{
    STEREO_ERR("[NV3D] CreateImage failed");
    nv3d_destroy(sd);
    return false;
}

VkMemoryRequirements mr;
sd->real.GetImageMemoryRequirements(
    sd->real_device,
    sd->nv3d_image,
    &mr);

VkImportMemoryWin32HandleInfoKHR import_mem = {
    .sType      =
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
    .handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
    .handle     = mem_handle,
};

VkMemoryAllocateInfo mai = {
    .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext           = &import_mem,
    .allocationSize  = mr.size,
    .memoryTypeIndex = 0,
};

if (sd->real.AllocateMemory(
        sd->real_device,
        &mai,
        NULL,
        &sd->nv3d_memory) != VK_SUCCESS)
{
    STEREO_ERR("[NV3D] AllocateMemory failed");
    nv3d_destroy(sd);
    return false;
}

if (sd->real.BindImageMemory(
        sd->real_device,
        sd->nv3d_image,
        sd->nv3d_memory,
        0) != VK_SUCCESS)
{
    STEREO_ERR("[NV3D] BindImageMemory failed");
    nv3d_destroy(sd);
    return false;
}

/* ------------------------------------------------------------- */
/* Imported timeline semaphore                                   */
/* ------------------------------------------------------------- */

VkSemaphoreTypeCreateInfo sem_type = {
    .sType         =
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    .initialValue  = 0,
};

VkSemaphoreCreateInfo sci = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    .pNext = &sem_type,
};

if (sd->real.CreateSemaphore(
        sd->real_device,
        &sci,
        NULL,
        &sd->nv3d_timeline) != VK_SUCCESS)
{
    STEREO_ERR("[NV3D] CreateSemaphore failed");
    nv3d_destroy(sd);
    return false;
}

VkImportSemaphoreWin32HandleInfoKHR import_sem = {
    .sType =
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
    .semaphore =
        sd->nv3d_timeline,
    .handleType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
    .handle =
        fence_handle,
};

if (sd->real.ImportSemaphoreWin32HandleKHR(
        sd->real_device,
        &import_sem) != VK_SUCCESS)
{
    STEREO_ERR("[NV3D] ImportSemaphoreWin32HandleKHR failed");
    nv3d_destroy(sd);
    return false;
}

sd->nv3d_ok = true;

STEREO_LOG(
    "[NV3D] initialized %ux%u",
    width,
    height);

return true;

}

/* ------------------------------------------------------------------------- */

void nv3d_destroy(StereoDevice *sd)
{
if (!sd)
return;

if (sd->nv3d_timeline)
{
    sd->real.DestroySemaphore(
        sd->real_device,
        sd->nv3d_timeline,
        NULL);

    sd->nv3d_timeline = VK_NULL_HANDLE;
}

if (sd->nv3d_image)
{
    sd->real.DestroyImage(
        sd->real_device,
        sd->nv3d_image,
        NULL);

    sd->nv3d_image = VK_NULL_HANDLE;
}

if (sd->nv3d_memory)
{
    sd->real.FreeMemory(
        sd->real_device,
        sd->nv3d_memory,
        NULL);

    sd->nv3d_memory = VK_NULL_HANDLE;
}

if (sd->nv3d_iface)
{
    ((InterfaceVulkan*)sd->nv3d_iface)->Delete();
    sd->nv3d_iface = NULL;
}

sd->nv3d_ok = false;

}

/* ------------------------------------------------------------------------- */
/* Present                                                                    */
/* ------------------------------------------------------------------------- */

VkResult nv3d_present(
StereoDevice      *sd,
StereoSwapchain   *sc,
VkQueue            queue,
uint32_t           wait_sem_count,
const VkSemaphore *wait_sems)
{
if (!nv3d_init(
sd,
sc->app_width,
sc->app_height))
{
return VK_ERROR_INITIALIZATION_FAILED;
}

VkCommandBuffer cmd = sc->barrier_cmds[0];

sd->real.ResetCommandBuffer(cmd, 0);

VkCommandBufferBeginInfo begin = {
    .sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
};

sd->real.BeginCommandBuffer(cmd, &begin);

cmd_image_barrier(
    sd,
    cmd,
    sc->stereo_images[0],
    2,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    VK_ACCESS_TRANSFER_READ_BIT,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT);

cmd_image_barrier(
    sd,
    cmd,
    sd->nv3d_image,
    2,
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    0,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT);

VkImageCopy copy = {
    .srcSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .layerCount = 2,
    },
    .dstSubresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .layerCount = 2,
    },
    .extent = {
        sc->app_width,
        sc->app_height,
        1,
    },
};

sd->real.CmdCopyImage(
    cmd,
    sc->stereo_images[0],
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    sd->nv3d_image,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    1,
    &copy);

sd->real.EndCommandBuffer(cmd);

sd->nv3d_value++;

VkTimelineSemaphoreSubmitInfo tsi = {
    .sType =
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
    .signalSemaphoreValueCount = 1,
    .pSignalSemaphoreValues =
        &sd->nv3d_value,
};

VkSubmitInfo sub = {
    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .pNext                = &tsi,
    .waitSemaphoreCount   = wait_sem_count,
    .pWaitSemaphores      = wait_sems,
    .commandBufferCount   = 1,
    .pCommandBuffers      = &cmd,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores    = &sd->nv3d_timeline,
};

VkResult vr =
    sd->real.QueueSubmit(
        queue,
        1,
        &sub,
        VK_NULL_HANDLE);

if (vr != VK_SUCCESS)
    return vr;

HRESULT hr =
    ((InterfaceVulkan*)sd->nv3d_iface)
        ->Present(sd->nv3d_value);

if (FAILED(hr))
{
    STEREO_ERR(
        "[NV3D] Present failed: 0x%08x",
        (unsigned)hr);
}

return VK_SUCCESS;

}
