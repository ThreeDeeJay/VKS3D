/*
 * device.c — vkCreateDevice / vkDestroyDevice
 *
 * We intercept device creation to:
 *   1. Enable VK_KHR_multiview (for multiview render pass injection)
 *   2. Enable VK_KHR_external_memory + VK_KHR_external_memory_win32
 *      (for importing D3D11 shared NT-handle textures as VkImages)
 *   3. Enable VK_KHR_create_renderpass2 (if available, for RP2 multiview)
 *   4. Build real-ICD device dispatch table
 *   5. Record the graphics queue handle for barrier submits
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* Forward decls (stereo.c) */
StereoDevice *stereo_device_alloc(void);
void          stereo_device_free(VkDevice h);
void stereo_populate_device_dispatch(StereoDevice *sd, VkInstance real_inst);

/*
 * Candidate device extensions VKS3D wants to enable.
 */
static const char *STEREO_CANDIDATE_EXTS[] = {
    "VK_KHR_multiview",
    "VK_KHR_maintenance2",
    "VK_KHR_create_renderpass2",
    "VK_NVX_multiview_per_view_attributes",

    "VK_KHR_external_memory",
    "VK_KHR_external_memory_win32",

    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_win32",
};
#define STEREO_CANDIDATE_EXT_COUNT \
    (sizeof(STEREO_CANDIDATE_EXTS) / sizeof(STEREO_CANDIDATE_EXTS[0]))

static const char **stereo_filter_extensions(
    VkPhysicalDevice               physDev,
    PFN_vkEnumerateDeviceExtensionProperties enumDevExts,
    uint32_t                       api_version,
    const char * const            *app_exts,
    uint32_t                       app_ext_count,
    uint32_t                      *out_count)
{
    (void)api_version;
    uint32_t dev_count = 0;
    enumDevExts(physDev, NULL, &dev_count, NULL);
    VkExtensionProperties *dev_props = NULL;
    if (dev_count) {
        dev_props = malloc(dev_count * sizeof(VkExtensionProperties));
        if (dev_props)
            enumDevExts(physDev, NULL, &dev_count, dev_props);
    }

    const char **merged = malloc(
        (app_ext_count + STEREO_CANDIDATE_EXT_COUNT) * sizeof(char*));
    uint32_t total = 0;

    for (uint32_t i = 0; i < app_ext_count; i++)
        merged[total++] = app_exts[i];

    for (uint32_t e = 0; e < STEREO_CANDIDATE_EXT_COUNT; e++) {
        const char *ext = STEREO_CANDIDATE_EXTS[e];

        bool app_has = false;
        for (uint32_t i = 0; i < app_ext_count; i++) {
            if (app_exts[i] && !strcmp(app_exts[i], ext)) { app_has = true; break; }
        }
        if (app_has) continue;

        bool supported = false;
        for (uint32_t d = 0; d < dev_count && dev_props; d++) {
            if (!strcmp(dev_props[d].extensionName, ext)) { supported = true; break; }
        }
        if (!supported) continue;

        merged[total++] = ext;
        STEREO_LOG("Injecting device extension: %s", ext);
    }

    free(dev_props);
    *out_count = total;
    return merged;
}

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

    VkPhysicalDeviceMemoryProperties mp;
    sd->si->real.GetPhysicalDeviceMemoryProperties(sd->real_physdev, &mp);

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
    OutputDebugStringA("[VKS3D] stereo_CreateDevice: ENTERED\n");
    STEREO_LOG("stereo_CreateDevice: called physicalDevice=%p (wrapper)", (void*)physicalDevice);

    StereoPhysdev   *sp          = (StereoPhysdev *)(uintptr_t)physicalDevice;
    VkPhysicalDevice real_physdev = sp->real_pd;
    StereoInstance  *sp_si        = sp->si;
    if (!sp_si) {
        STEREO_ERR("stereo_CreateDevice: wrapper %p has NULL si (real_pd=%p)",
                   (void*)physicalDevice, (void*)real_physdev);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    STEREO_LOG("stereo_CreateDevice: wrapper=%p real_pd=%p si=%p",
               (void*)physicalDevice, (void*)real_physdev, (void*)sp_si);

    VkPhysicalDeviceMultiviewFeatures multiview_feat = {
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
        .multiview                  = VK_TRUE,
        .multiviewGeometryShader    = VK_TRUE,  /* needed: GS in multiview render pass */
        .multiviewTessellationShader = VK_TRUE, /* needed: injected TCS+TES in multiview */
    };

    /* Enable tessellationShader in base features so we can inject TCS+TES
     * into VS-only pipelines.  We merge with whatever the app requested. */
    VkPhysicalDeviceFeatures base_feats = {0};
    if (pCreateInfo->pEnabledFeatures)
        base_feats = *pCreateInfo->pEnabledFeatures;
    base_feats.tessellationShader = VK_TRUE;

    bool has_mv_feat = false;
    {
        VkBaseOutStructure *node = (VkBaseOutStructure*)pCreateInfo->pNext;
        while (node) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
                ((VkPhysicalDeviceMultiviewFeatures*)node)->multiview = VK_TRUE;
                has_mv_feat = true;
                break;
            }
            node = node->pNext;
        }
    }
    if (!has_mv_feat)
        multiview_feat.pNext = (void*)pCreateInfo->pNext;

    VkDeviceCreateInfo dci = *pCreateInfo;
    if (!has_mv_feat)
        dci.pNext = &multiview_feat;
    dci.pEnabledFeatures = &base_feats;

    VkPhysicalDeviceProperties phys_props;
    sp_si->real.GetPhysicalDeviceProperties(real_physdev, &phys_props);
    uint32_t dev_api = phys_props.apiVersion;

    PFN_vkEnumerateDeviceExtensionProperties enumDevExts =
        (PFN_vkEnumerateDeviceExtensionProperties)(uintptr_t)
        sp_si->real_get_instance_proc_addr(
            sp_si->real_instance, "vkEnumerateDeviceExtensionProperties");

    uint32_t total_exts = 0;
    const char **new_exts = enumDevExts
        ? stereo_filter_extensions(real_physdev, enumDevExts, dev_api,
                                    dci.ppEnabledExtensionNames,
                                    dci.enabledExtensionCount,
                                    &total_exts)
        : NULL;

    if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;

    dci.enabledExtensionCount   = total_exts;
    dci.ppEnabledExtensionNames = (const char* const*)new_exts;

    VkDevice real_dev = VK_NULL_HANDLE;
    VkResult res = sp_si->real.CreateDevice(
        real_physdev, &dci, pAllocator, &real_dev);
    free(new_exts);

    if (res != VK_SUCCESS) {
        STEREO_LOG("Real CreateDevice failed: %d", res);
        return res;
    }

    StereoDevice *sd = stereo_device_alloc();
    if (!sd) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    SET_LOADER_MAGIC_VALUE(sd);

    sd->real_device = real_dev;
    sd->si          = sp_si;
    sd->real_physdev = real_physdev;
    sd->stereo      = sp_si->stereo;

    stereo_populate_device_dispatch(sd, sp_si->real_instance);

    {
        uint32_t qf_count = 0;
        sp_si->real.GetPhysicalDeviceQueueFamilyProperties(real_physdev, &qf_count, NULL);
        VkQueueFamilyProperties *qfps = malloc(qf_count * sizeof(VkQueueFamilyProperties));
        if (qfps) {
            sp_si->real.GetPhysicalDeviceQueueFamilyProperties(real_physdev, &qf_count, qfps);
            for (uint32_t i = 0; i < qf_count; i++) {
                if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    sd->gfx_qf = i;
                    sd->real.GetDeviceQueue(real_dev, i, 0, &sd->gfx_queue);
                    break;
                }
            }
            free(qfps);
        }
    }

    if (sd->stereo.enabled) {
        res = create_stereo_ubo(sd);
        if (res != VK_SUCCESS) {
            STEREO_ERR("Failed to create stereo UBO: %d", res);
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

    /* Destroy pooled temporary patched shader modules.
     *
     * Driver 426.06 retains a reference to the VkShaderModule SPIR-V even
     * after vkCreateGraphicsPipelines returns, so we must not destroy them
     * immediately after pipeline creation (doing so causes the driver to
     * silently fall back to unpatched code, producing mono output).
     * We pool them in sd->tmp_modules[] and release them all here, just
     * before the device itself is destroyed — at that point the driver has
     * no further use for the modules. */
    STEREO_LOG("stereo_DestroyDevice: releasing %u pooled tmp shader modules",
               sd->tmp_module_count);
    for (uint32_t i = 0; i < sd->tmp_module_count; i++) {
        if (sd->tmp_modules[i])
            sd->real.DestroyShaderModule(sd->real_device, sd->tmp_modules[i], NULL);
    }
    sd->tmp_module_count = 0;

    /* Release shader SPIR-V cache */
    for (uint32_t i = 0; i < sd->shader_cache_count; i++)
        free(sd->shader_cache[i].spv);
    sd->shader_cache_count = 0;

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