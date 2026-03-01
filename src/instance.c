/*
 * instance.c — vkCreateInstance, vkDestroyInstance, enumeration
 *
 * On CreateInstance we:
 *   1. Load the real GPU ICD
 *   2. Inject VK_KHR_get_physical_device_properties2 (needed for multiview)
 *   3. Create the real instance
 *   4. Populate our dispatch tables
 *   5. Allocate our StereoInstance wrapper
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* Extensions we transparently add to every instance */
static const char *STEREO_EXTRA_INSTANCE_EXTS[] = {
    "VK_KHR_get_physical_device_properties2",
};
#define STEREO_EXTRA_INST_EXT_COUNT \
    (sizeof(STEREO_EXTRA_INSTANCE_EXTS) / sizeof(STEREO_EXTRA_INSTANCE_EXTS[0]))

/* Forward declarations (defined in stereo.c) */
bool stereo_load_real_icd(void);
PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void);
StereoInstance *stereo_instance_alloc(void);
void stereo_populate_instance_dispatch(StereoInstance *si);
StereoPhysicalDevice *stereo_physdev_alloc(void);

/* ── vkEnumerateInstanceVersion ─────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
    if (!stereo_load_real_icd()) {
        *pApiVersion = VK_API_VERSION_1_1;
        return VK_SUCCESS;
    }
    PFN_vkGetInstanceProcAddr giPA = stereo_get_real_giPA();
    PFN_vkEnumerateInstanceVersion eiv =
        (PFN_vkEnumerateInstanceVersion)(uintptr_t)giPA(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (eiv)
        return eiv(pApiVersion);
    *pApiVersion = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

/* ── vkEnumerateInstanceExtensionProperties ─────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumerateInstanceExtensionProperties(
    const char          *pLayerName,
    uint32_t            *pPropertyCount,
    VkExtensionProperties *pProperties)
{
    if (!stereo_load_real_icd())
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr giPA = stereo_get_real_giPA();
    PFN_vkEnumerateInstanceExtensionProperties eiep =
        (PFN_vkEnumerateInstanceExtensionProperties)
        giPA(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!eiep)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* First get the real list */
    uint32_t real_count = 0;
    VkResult res = eiep(pLayerName, &real_count, NULL);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return res;

    /* Count how many extras we need to add */
    /* (Allocate temp buffer for real extensions) */
    VkExtensionProperties *real_props = NULL;
    if (real_count > 0) {
        real_props = malloc(real_count * sizeof(VkExtensionProperties));
        if (!real_props) return VK_ERROR_OUT_OF_HOST_MEMORY;
        res = eiep(pLayerName, &real_count, real_props);
    }

    /* Build merged list */
    uint32_t total = real_count;
    /* Check which extras are already present */
    for (uint32_t e = 0; e < STEREO_EXTRA_INST_EXT_COUNT; e++) {
        bool found = false;
        for (uint32_t r = 0; r < real_count; r++) {
            if (!strcmp(real_props[r].extensionName, STEREO_EXTRA_INSTANCE_EXTS[e])) {
                found = true; break;
            }
        }
        if (!found) total++;
    }

    if (!pProperties) {
        *pPropertyCount = total;
        free(real_props);
        return VK_SUCCESS;
    }

    uint32_t write = 0;
    /* Copy real extensions */
    for (uint32_t r = 0; r < real_count && write < *pPropertyCount; r++) {
        pProperties[write++] = real_props[r];
    }
    /* Append our extras if not already present */
    for (uint32_t e = 0; e < STEREO_EXTRA_INST_EXT_COUNT && write < *pPropertyCount; e++) {
        bool found = false;
        for (uint32_t r = 0; r < real_count; r++) {
            if (!strcmp(real_props[r].extensionName, STEREO_EXTRA_INSTANCE_EXTS[e])) {
                found = true; break;
            }
        }
        if (!found) {
            strncpy(pProperties[write].extensionName,
                    STEREO_EXTRA_INSTANCE_EXTS[e], VK_MAX_EXTENSION_NAME_SIZE);
            pProperties[write].specVersion = 1;
            write++;
        }
    }

    free(real_props);
    *pPropertyCount = write;
    return (write < total) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ── vkCreateInstance ───────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateInstance(
    const VkInstanceCreateInfo    *pCreateInfo,
    const VkAllocationCallbacks   *pAllocator,
    VkInstance                    *pInstance)
{
    if (!stereo_load_real_icd())
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr giPA = stereo_get_real_giPA();
    PFN_vkCreateInstance real_ci =
        (PFN_vkCreateInstance)giPA(VK_NULL_HANDLE, "vkCreateInstance");
    if (!real_ci)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* Build modified create info with our extra extensions */
    VkInstanceCreateInfo ci = *pCreateInfo;
    uint32_t orig_count = ci.enabledExtensionCount;
    const char **new_exts = malloc((orig_count + STEREO_EXTRA_INST_EXT_COUNT)
                                   * sizeof(char*));
    if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* Copy original extensions */
    for (uint32_t i = 0; i < orig_count; i++)
        new_exts[i] = ci.ppEnabledExtensionNames[i];

    /* Append stereo extras if not already there */
    uint32_t total = orig_count;
    for (uint32_t e = 0; e < STEREO_EXTRA_INST_EXT_COUNT; e++) {
        bool found = false;
        for (uint32_t i = 0; i < orig_count; i++) {
            if (ci.ppEnabledExtensionNames[i] &&
                !strcmp(ci.ppEnabledExtensionNames[i], STEREO_EXTRA_INSTANCE_EXTS[e])) {
                found = true; break;
            }
        }
        if (!found)
            new_exts[total++] = STEREO_EXTRA_INSTANCE_EXTS[e];
    }

    ci.enabledExtensionCount    = total;
    ci.ppEnabledExtensionNames  = new_exts;

    /* Ensure Vulkan 1.1 */
    VkApplicationInfo app_info;
    uint32_t app_ver = VK_API_VERSION_1_0;
    if (ci.pApplicationInfo) {
        app_info = *ci.pApplicationInfo;
        app_ver  = app_info.apiVersion;
    } else {
        memset(&app_info, 0, sizeof(app_info));
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    }
    if (app_ver < VK_API_VERSION_1_1)
        app_info.apiVersion = VK_API_VERSION_1_1;
    ci.pApplicationInfo = &app_info;

    VkInstance real_inst = VK_NULL_HANDLE;
    VkResult res = real_ci(&ci, pAllocator, &real_inst);
    free(new_exts);

    if (res != VK_SUCCESS)
        return res;

    /* Allocate our wrapper */
    StereoInstance *si = stereo_instance_alloc();
    if (!si) {
        /* Clean up real instance */
        PFN_vkDestroyInstance di =
            (PFN_vkDestroyInstance)giPA(real_inst, "vkDestroyInstance");
        if (di) di(real_inst, pAllocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    si->real_instance              = real_inst;
    si->real_get_instance_proc_addr = giPA;
    stereo_config_init(&si->stereo);
    stereo_populate_instance_dispatch(si);

    /* Return real handle — loader uses dispatch key at start of handle */
    *pInstance = real_inst;

    STEREO_LOG("Instance created: %p (stereo: %s)",
               (void*)real_inst, si->stereo.enabled ? "ON" : "OFF");
    return VK_SUCCESS;
}

/* ── vkDestroyInstance ──────────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    StereoInstance *si = stereo_instance_from_handle(instance);
    if (!si) return;

    si->real.DestroyInstance(si->real_instance, pAllocator);
    stereo_instance_free(instance);
}

/* ── vkEnumeratePhysicalDevices ─────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumeratePhysicalDevices(
    VkInstance         instance,
    uint32_t          *pPhysicalDeviceCount,
    VkPhysicalDevice  *pPhysicalDevices)
{
    StereoInstance *si = stereo_instance_from_handle(instance);
    if (!si) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = si->real.EnumeratePhysicalDevices(
        si->real_instance, pPhysicalDeviceCount, pPhysicalDevices);

    if (res == VK_SUCCESS && pPhysicalDevices) {
        /* Register any new physical devices */
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
            if (!stereo_physdev_from_handle(pPhysicalDevices[i])) {
                StereoPhysicalDevice *sp = stereo_physdev_alloc();
                if (sp) {
                    sp->real     = pPhysicalDevices[i];
                    sp->instance = si;
                }
            }
        }
    }

    return res;
}
