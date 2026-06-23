#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <malloc.h>

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

static uint32_t find_memory_type(
    StereoDevice *sd,
    uint32_t type_bits,
    VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(
        sd->real_physdev,
        &mp);

    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }

    return UINT32_MAX;
}

extern "C" bool nv3d_init(
    StereoDevice *sd,
    uint32_t width,
    uint32_t height)
{
STEREO_LOG(
    "[NV3D] nv3d_init(%u x %u)",
    width,
    height);

if (sd->nv3d_ok &&
    sd->nv3d_width  == width * 2 &&
    sd->nv3d_height == height)
{
    return true;
}

nv3d_destroy(sd);

NV3D::InterfaceVulkan *iface = nullptr;

NV3D::InitParams params = {};

STEREO_LOG(
    "[NV3D] instance=%p physdev=%p device=%p gfx_qf=%u",
    sd->si->real_instance,
    sd->real_physdev,
    sd->real_device,
    sd->gfx_qf);

VkPhysicalDeviceProperties props = {};

sd->si->real.GetPhysicalDeviceProperties(
    sd->real_physdev,
    &props);

STEREO_LOG(
    "[NV3D] GPU=%s vendor=%04X device=%04X",
    props.deviceName,
    props.vendorID,
    props.deviceID);

STEREO_LOG(
    "[NV3D] si=%p",
    sd->si);

STEREO_LOG(
    "[NV3D] real_instance=%p",
    sd->si->real_instance);

STEREO_LOG(
    "[NV3D] real_physdev=%p",
    sd->real_physdev);

STEREO_LOG(
    "[NV3D] real_device=%p",
    sd->real_device);

NV3D::SetLogSink(
    [](NV3D::LogLevel lvl, const wchar_t* msg, void*)
    {
        char utf8[4096];

        WideCharToMultiByte(
            CP_UTF8,
            0,
            msg,
            -1,
            utf8,
            sizeof(utf8),
            nullptr,
            nullptr);

        STEREO_LOG("[NV3D] %s", utf8);
    },
    nullptr);


STEREO_LOG(
    "[NV3D] real_instance=%p wrapper=%p",
    sd->si->real_instance,
    sd->si);

HMODULE vulkan =
    GetModuleHandleW(L"vulkan-1.dll");

STEREO_LOG(
    "[NV3D] VKS3D vulkan-1.dll=%p",
    vulkan);

VkPhysicalDeviceIDProperties id{};
id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

VkPhysicalDeviceProperties2 props2{};
props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
props2.pNext = &id;

sd->si->real.GetPhysicalDeviceProperties2(
    sd->real_physdev,
    &props2);

if (id.deviceLUIDValid)
{
    const uint8_t* l = id.deviceLUID;

    STEREO_LOG(
        "[NV3D TEST] Vulkan LUID=%02X%02X%02X%02X-%02X%02X%02X%02X",
        l[0], l[1], l[2], l[3],
        l[4], l[5], l[6], l[7]);

    params.has_external_luid = true;
    static_assert(sizeof(LUID) == VK_LUID_SIZE, "LUID mismatch");
    std::memcpy(&params.external_luid, id.deviceLUID, VK_LUID_SIZE);

    STEREO_LOG("[NV3D] passing external LUID to NV3D-Lib");
}

HRESULT hr =
    NV3D::CreateInterfaceVulkan(
        sd->si->real_instance,
        sd->real_physdev,
        sd->real_device,
        sd->gfx_qf,
        &params,
        &iface);

STEREO_LOG(
    "[NV3D] CreateInterfaceVulkan hr=0x%08X iface=%p",
    (unsigned)hr,
    iface);

if (FAILED(hr) || !iface)
{
    STEREO_ERR(
        "[NV3D] CreateInterfaceVulkan failed: 0x%08x",
        (unsigned)hr);
    return false;
}

HANDLE mem_handle   = NULL;
HANDLE fence_handle = NULL;

hr =
    iface->InitSharedResources(
        width * 2,
        height,
        87, /* DXGI_FORMAT_B8G8R8A8_UNORM */
        &mem_handle,
        &fence_handle);
STEREO_LOG(
    "[NV3D] InitSharedResources hr=0x%08X",
    (unsigned)hr);

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
sd->nv3d_width        = width * 2;
sd->nv3d_height       = height;
sd->nv3d_value        = 0;

if (FAILED(hr))
{
    STEREO_ERR(
        "[NV3D] InitSharedResources failed: 0x%08x",
        (unsigned)hr);

    iface->Delete();
    return false;
}

STEREO_LOG(
    "[NV3D] imported image %ux%u",
    sd->nv3d_width,
    sd->nv3d_height);

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
    .extent        = { width * 2, height, 1 },
    .mipLevels     = 1,
    .arrayLayers   = 1,
    .samples       = VK_SAMPLE_COUNT_1_BIT,
    .tiling        = VK_IMAGE_TILING_OPTIMAL,
    .usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT,
    .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
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

uint32_t mem_type =
    find_memory_type(sd,
                     mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

if (mem_type == UINT32_MAX)
{
    STEREO_ERR("[NV3D] no compatible memory type");
    nv3d_destroy(sd);
    return false;
}

VkImportMemoryWin32HandleInfoKHR import_info = {
    .sType      =
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
    .handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
    .handle     = mem_handle
};

VkMemoryAllocateInfo ai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = &import_info,
    .allocationSize  = mr.size,
    .memoryTypeIndex = mem_type,
};

if (sd->real.AllocateMemory(
        sd->real_device,
        &ai,
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

if (!sd->real.ImportSemaphoreWin32HandleKHR)
{
    STEREO_ERR(
        "[NV3D] ImportSemaphoreWin32HandleKHR not loaded");
    nv3d_destroy(sd);
    return false;
}

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
    "[NV3D] initialized imported texture %ux%u",
    width * 2,
    height);

return true;

}

/* ------------------------------------------------------------------------- */

extern "C" void nv3d_destroy(
    StereoDevice *sd)
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
    ((NV3D::InterfaceVulkan*)sd->nv3d_iface)->Delete();
    sd->nv3d_iface = NULL;
}

sd->nv3d_ok = false;

sd->nv3d_width  = 0;
sd->nv3d_height = 0;
sd->nv3d_value  = 0;
}

/* ------------------------------------------------------------------------- */
/* Present                                                                    */
/* ------------------------------------------------------------------------- */

extern "C" VkResult nv3d_present(
    StereoDevice *sd,
    StereoSwapchain *sc,
    VkQueue queue,
    uint32_t wait_sem_count,
    const VkSemaphore *wait_sems)
{
STEREO_LOG("[PRESENT] using NV3D path");
STEREO_LOG(
    "[NV3D] present iface=%p image=%p timeline=%p value=%llu",
    sd->nv3d_iface,
    sd->nv3d_image,
    sd->nv3d_timeline,
    (unsigned long long)sd->nv3d_value);
if (!sd->nv3d_iface)
{
    STEREO_LOG(
        "[NV3D] present requested but nv3d_iface=NULL");

    return VK_ERROR_INITIALIZATION_FAILED;
}

if (!sc->barrier_cmds)
{
    STEREO_ERR(
        "[NV3D] barrier_cmds array is NULL");
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkCommandBuffer cmd = sc->barrier_cmds[0];

if (!cmd)
{
    STEREO_ERR(
        "[NV3D] barrier_cmds[0] is NULL");
    return VK_ERROR_INITIALIZATION_FAILED;
}

STEREO_LOG(
    "[NV3D] cmd=%p stereoImage=%p",
    cmd,
    sc->stereo_images ?
        sc->stereo_images[0] :
        VK_NULL_HANDLE);

if (cmd == VK_NULL_HANDLE)
{
    STEREO_ERR(
        "[NV3D] barrier_cmds[0] is NULL");

    return VK_ERROR_INITIALIZATION_FAILED;
}

sd->real.ResetCommandBuffer(cmd, 0);

VkCommandBufferBeginInfo begin = {
    .sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
};

sd->real.BeginCommandBuffer(cmd, &begin);

/* stereo render target -> transfer source */
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

/* NV3D imported texture -> transfer destination */
cmd_image_barrier(
    sd,
    cmd,
    sd->nv3d_image,
    1,
    VK_IMAGE_LAYOUT_GENERAL,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    0,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT);

VkImageCopy copies[2];
std::memset(copies, 0, sizeof(copies));

/* left eye -> left half */
copies[0].srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
copies[0].srcSubresource.baseArrayLayer = 0;
copies[0].srcSubresource.layerCount     = 1;

copies[0].dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
copies[0].dstSubresource.baseArrayLayer = 0;
copies[0].dstSubresource.layerCount     = 1;

copies[0].extent.width  = sc->app_width;
copies[0].extent.height = sc->app_height;
copies[0].extent.depth  = 1;

/* right eye -> right half */
copies[1] = copies[0];

copies[1].srcSubresource.baseArrayLayer = 1;
copies[1].dstOffset.x                   = (int32_t)sc->app_width;

sd->real.CmdCopyImage(
    cmd,
    sc->stereo_images[0],
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    sd->nv3d_image,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    2,
    copies);

/* return imported texture to GENERAL for NV3D-Lib */
cmd_image_barrier(
    sd,
    cmd,
    sd->nv3d_image,
    1,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    VK_IMAGE_LAYOUT_GENERAL,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_ACCESS_MEMORY_READ_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

cmd_image_barrier(
    sd,
    cmd,
    sc->stereo_images[0],
    2,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_ACCESS_TRANSFER_READ_BIT,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

sd->real.EndCommandBuffer(cmd);

sd->nv3d_value++;

VkTimelineSemaphoreSubmitInfo tsi = {
    .sType =
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
    .signalSemaphoreValueCount = 1,
    .pSignalSemaphoreValues =
        &sd->nv3d_value,
};

VkPipelineStageFlags *waitStages =
    (VkPipelineStageFlags*)alloca(
        sizeof(VkPipelineStageFlags) * wait_sem_count);

for (uint32_t i = 0; i < wait_sem_count; i++)
{
    waitStages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
}

VkSubmitInfo sub = {
    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .pNext                = &tsi,
    .waitSemaphoreCount   = wait_sem_count,
    .pWaitSemaphores      = wait_sems,
    .pWaitDstStageMask    = waitStages,
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

STEREO_LOG(
    "[NV3D] Present begin value=%llu iface=%p",
    (unsigned long long)sd->nv3d_value,
    sd->nv3d_iface);

HRESULT hr =
    ((NV3D::InterfaceVulkan*)sd->nv3d_iface)
        ->Present(sd->nv3d_value);

STEREO_LOG(
    "[NV3D] Present end hr=0x%08X",
    (unsigned)hr);

if (FAILED(hr))
{
    STEREO_ERR(
        "[NV3D] Present failed: 0x%08x",
        (unsigned)hr);
}

return VK_SUCCESS;

}
