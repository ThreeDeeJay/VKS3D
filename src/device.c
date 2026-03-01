/*
 * device.c — vkCreateDevice / vkDestroyDevice
 *
 * We intercept device creation to:
 *   1. Enable VkPhysicalDeviceMultiviewFeatures (VK_KHR_multiview)
 *   2. Enable VK_KHR_multiview device extension
 *   3. Enable VK_KHR_swapchain for SBS output
 *   4. Allocate the per-device stereo UBO (eye offsets + convergence)
 *   5. Build real-ICD device dispatch table
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* Forward decls (stereo.c) */
StereoDevice *stereo_device_alloc(void);
void          stereo_device_free(VkDevice h);
void stereo_populate_device_dispatch(StereoDevice *sd, VkInstance real_inst);
extern StereoPhysicalDevice *stereo_physdev_from_handle(VkPhysicalDevice h);

/* Extra device extensions we inject */
static const char *STEREO_EXTRA_DEV_EXTS[] = {
    "VK_KHR_multiview",
    "VK_KHR_maintenance2",   /* needed by multiview on some drivers */
};
#define STEREO_EXTRA_DEV_EXT_COUNT \
    (sizeof(STEREO_EXTRA_DEV_EXTS) / sizeof(STEREO_EXTRA_DEV_EXTS[0]))

/* ── Allocate and bind stereo UBO ───────────────────────────────────────── */
static VkResult create_stereo_ubo(StereoDevice *sd)
{
    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = sizeof(StereoUBO),
        .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult res = sd->real.CreateBuffer(sd->real_device, &bci, NULL, &sd->stereo_ubo);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements mr;
    sd->real.GetBufferMemoryRequirements(sd->real_device, sd->stereo_ubo, &mr);

    /* Find HOST_VISIBLE | HOST_COHERENT memory type */
    VkPhysicalDeviceMemoryProperties mp;
    sd->phys_dev->instance->real.GetPhysicalDeviceMemoryProperties(
        sd->phys_dev->real, &mp);

    uint32_t mem_type = UINT32_MAX;
    VkMemoryPropertyFlags needed =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & needed) == needed) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) {
        STEREO_ERR("No suitable memory for stereo UBO");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mr.size,
        .memoryTypeIndex = mem_type,
    };
    res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, &sd->stereo_ubo_mem);
    if (res != VK_SUCCESS) return res;

    res = sd->real.BindBufferMemory(sd->real_device, sd->stereo_ubo, sd->stereo_ubo_mem, 0);
    if (res != VK_SUCCESS) return res;

    res = sd->real.MapMemory(sd->real_device, sd->stereo_ubo_mem,
                              0, sizeof(StereoUBO), 0, &sd->stereo_ubo_map);
    if (res != VK_SUCCESS) return res;

    /* Write initial values */
    StereoUBO *ubo = (StereoUBO*)sd->stereo_ubo_map;
    ubo->eye_offset[0] = sd->stereo.left_eye_offset;
    ubo->eye_offset[1] = sd->stereo.right_eye_offset;
    ubo->convergence   = sd->stereo.convergence;
    ubo->_pad          = 0.0f;

    STEREO_LOG("Stereo UBO: left=%.4f right=%.4f conv=%.4f",
               ubo->eye_offset[0], ubo->eye_offset[1], ubo->convergence);
    return VK_SUCCESS;
}

/* ── vkCreateDevice ─────────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateDevice(
    VkPhysicalDevice              physicalDevice,
    const VkDeviceCreateInfo     *pCreateInfo,
    const VkAllocationCallbacks  *pAllocator,
    VkDevice                     *pDevice)
{
    StereoPhysicalDevice *sp = stereo_physdev_from_handle(physicalDevice);
    if (!sp) return VK_ERROR_INITIALIZATION_FAILED;

    /* ── Inject VkPhysicalDeviceMultiviewFeatures ─────────────────────── */
    VkPhysicalDeviceMultiviewFeatures multiview_feat = {
        .sType                         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
        .pNext                         = NULL,
        .multiview                     = VK_TRUE,
        .multiviewGeometryShader       = VK_FALSE,
        .multiviewTessellationShader   = VK_FALSE,
    };

    /* Walk existing pNext chain to check if multiview features already set */
    void *next_head = (void*)pCreateInfo->pNext;
    bool  has_mv_feat = false;
    {
        VkBaseOutStructure *node = (VkBaseOutStructure*)next_head;
        while (node) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
                /* App already set it — just ensure multiview=true */
                ((VkPhysicalDeviceMultiviewFeatures*)node)->multiview = VK_TRUE;
                has_mv_feat = true;
                break;
            }
            node = node->pNext;
        }
    }
    if (!has_mv_feat) {
        /* Prepend our multiview features to the chain */
        multiview_feat.pNext = (void*)pCreateInfo->pNext;
    }

    /* ── Build modified VkDeviceCreateInfo ───────────────────────────── */
    VkDeviceCreateInfo dci = *pCreateInfo;
    if (!has_mv_feat)
        dci.pNext = &multiview_feat;

    /* Inject extra extensions */
    uint32_t orig_ext_count = dci.enabledExtensionCount;
    const char **new_exts   = malloc((orig_ext_count + STEREO_EXTRA_DEV_EXT_COUNT)
                                      * sizeof(char*));
    if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < orig_ext_count; i++)
        new_exts[i] = dci.ppEnabledExtensionNames[i];

    uint32_t total_exts = orig_ext_count;
    for (uint32_t e = 0; e < STEREO_EXTRA_DEV_EXT_COUNT; e++) {
        bool found = false;
        for (uint32_t i = 0; i < orig_ext_count; i++) {
            if (dci.ppEnabledExtensionNames[i] &&
                !strcmp(dci.ppEnabledExtensionNames[i], STEREO_EXTRA_DEV_EXTS[e])) {
                found = true; break;
            }
        }
        if (!found) new_exts[total_exts++] = STEREO_EXTRA_DEV_EXTS[e];
    }
    dci.enabledExtensionCount   = total_exts;
    dci.ppEnabledExtensionNames = (const char* const*)new_exts;

    /* ── Create real device ──────────────────────────────────────────── */
    VkDevice real_dev = VK_NULL_HANDLE;
    VkResult res = sp->instance->real.CreateDevice(
        sp->real, &dci, pAllocator, &real_dev);
    free(new_exts);

    if (res != VK_SUCCESS) {
        STEREO_LOG("Real CreateDevice failed: %d", res);
        return res;
    }

    /* ── Allocate our wrapper ────────────────────────────────────────── */
    StereoDevice *sd = stereo_device_alloc();
    if (!sd) {
        /* TODO: destroy real device */
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    sd->real_device = real_dev;
    sd->phys_dev    = sp;
    sd->stereo      = sp->instance->stereo;

    stereo_populate_device_dispatch(sd, sp->instance->real_instance);

    /* Allocate stereo UBO */
    if (sd->stereo.enabled) {
        res = create_stereo_ubo(sd);
        if (res != VK_SUCCESS) {
            STEREO_ERR("Failed to create stereo UBO: %d", res);
            /* Non-fatal: continue without per-draw UBO updates */
        }
    }

    *pDevice = real_dev;
    STEREO_LOG("Device created: %p", (void*)real_dev);
    return VK_SUCCESS;
}

/* ── vkDestroyDevice ────────────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;

    /* Cleanup stereo UBO */
    if (sd->stereo_ubo != VK_NULL_HANDLE) {
        if (sd->stereo_ubo_map)
            sd->real.UnmapMemory(sd->real_device, sd->stereo_ubo_mem);
        sd->real.DestroyBuffer(sd->real_device, sd->stereo_ubo, NULL);
        sd->real.FreeMemory(sd->real_device, sd->stereo_ubo_mem, NULL);
    }

    sd->real.DestroyDevice(sd->real_device, pAllocator);
    stereo_device_free(device);
    STEREO_LOG("Device destroyed: %p", (void*)device);
}
