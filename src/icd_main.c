/*
 * icd_main.c — Vulkan ICD entry points
 *
 * The Vulkan loader calls:
 *   1. vk_icdNegotiateLoaderICDInterfaceVersion   — agree on ABI version
 *   2. vk_icdGetInstanceProcAddr(NULL, ...)        — pre-instance queries
 *   3. vk_icdGetInstanceProcAddr(instance, ...)    — all other lookups
 *
 * We return our wrapped function pointers for intercepted functions
 * and passthrough pointers for everything else.
 */

#include <stdio.h>
#include <string.h>
#include "stereo_icd.h"

/* ── Global real-ICD get-instance-proc-addr ─────────────────────────────── */
/* Set during CreateInstance from the loaded real ICD */
/* Forward declarations for real-ICD proc-addr getters (stereo.c) */
PFN_vkGetInstanceProcAddr stereo_get_real_pdPA(void);

/* ── Helper: map function name → our wrapped pointer ─────────────────────── */
static PFN_vkVoidFunction get_instance_proc_addr_internal(
    VkInstance instance, const char *name)
{
    /* Pre-instance commands */
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)stereo_CreateInstance;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)stereo_EnumerateInstanceExtensionProperties;
    if (!strcmp(name, "vkEnumerateInstanceVersion"))
        return (PFN_vkVoidFunction)stereo_EnumerateInstanceVersion;

    if (!instance)
        return NULL;

    StereoInstance *si = stereo_instance_from_handle(instance);

    /* ── Instance-level commands ─────────────────────────────────────────── */
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)stereo_DestroyInstance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)stereo_EnumeratePhysicalDevices;

    /*
     * Physical device / surface passthroughs.
     *
     * PASSTHROUGH_INST only returns the cached pointer when it is non-NULL.
     * When the cached pointer is NULL (common for surface functions — many
     * ICDs do not expose them via vk_icdGetInstanceProcAddr because the
     * loader manages surface objects through terminators) we fall through
     * to the dynamic lookup at the bottom of this function.
     * Without this, returning NULL here causes the loader's stub to fire
     * and return VK_ERROR_INITIALIZATION_FAILED to the application.
     */
#define PASSTHROUGH_INST(fn)     if (!strcmp(name, "vk"#fn)) {         if (si && si->real.fn)             return (PFN_vkVoidFunction)si->real.fn;         /* fall through to dynamic lookup */         goto dynamic_lookup;     }

    PASSTHROUGH_INST(GetPhysicalDeviceProperties)
    PASSTHROUGH_INST(GetPhysicalDeviceProperties2)
    PASSTHROUGH_INST(GetPhysicalDeviceFeatures)
    PASSTHROUGH_INST(GetPhysicalDeviceFeatures2)
    PASSTHROUGH_INST(GetPhysicalDeviceMemoryProperties)
    PASSTHROUGH_INST(GetPhysicalDeviceMemoryProperties2)
    PASSTHROUGH_INST(GetPhysicalDeviceQueueFamilyProperties)
    PASSTHROUGH_INST(GetPhysicalDeviceQueueFamilyProperties2)
    PASSTHROUGH_INST(GetPhysicalDeviceFormatProperties)
    PASSTHROUGH_INST(GetPhysicalDeviceFormatProperties2)
    PASSTHROUGH_INST(GetPhysicalDeviceImageFormatProperties)
    PASSTHROUGH_INST(GetPhysicalDeviceSparseImageFormatProperties)
    PASSTHROUGH_INST(EnumerateDeviceExtensionProperties)
    PASSTHROUGH_INST(EnumerateDeviceLayerProperties)
    PASSTHROUGH_INST(DestroySurfaceKHR)
    PASSTHROUGH_INST(GetPhysicalDeviceSurfaceSupportKHR)
    PASSTHROUGH_INST(GetPhysicalDeviceSurfaceCapabilitiesKHR)
    PASSTHROUGH_INST(GetPhysicalDeviceSurfaceFormatsKHR)
    PASSTHROUGH_INST(GetPhysicalDeviceSurfacePresentModesKHR)
    PASSTHROUGH_INST(CreateDebugUtilsMessengerEXT)
    PASSTHROUGH_INST(DestroyDebugUtilsMessengerEXT)
#undef PASSTHROUGH_INST

    /* CreateDevice is wrapped */
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)stereo_CreateDevice;

    /* ── Device-level commands ───────────────────────────────────────────── */
    /* For device commands, the loader uses vkGetDeviceProcAddr which maps to
     * our stereo_GetDeviceProcAddr below.  We still handle them here for
     * callers that go through vkGetInstanceProcAddr for device funcs. */
    if (!strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)stereo_DestroyDevice;
    if (!strcmp(name, "vkCreateRenderPass"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass;
    if (!strcmp(name, "vkCreateRenderPass2KHR"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass2KHR;
    if (!strcmp(name, "vkCreateShaderModule"))
        return (PFN_vkVoidFunction)stereo_CreateShaderModule;
    if (!strcmp(name, "vkDestroyShaderModule"))
        return (PFN_vkVoidFunction)stereo_DestroyShaderModule;
    if (!strcmp(name, "vkCreateSwapchainKHR"))
        return (PFN_vkVoidFunction)stereo_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR"))
        return (PFN_vkVoidFunction)stereo_DestroySwapchainKHR;
    if (!strcmp(name, "vkGetSwapchainImagesKHR"))
        return (PFN_vkVoidFunction)stereo_GetSwapchainImagesKHR;
    if (!strcmp(name, "vkAcquireNextImageKHR"))
        return (PFN_vkVoidFunction)stereo_AcquireNextImageKHR;
    if (!strcmp(name, "vkQueuePresentKHR"))
        return (PFN_vkVoidFunction)stereo_QueuePresentKHR;

    /* Everything else (and fallthrough from PASSTHROUGH_INST when cached
     * pointer is NULL): forward to real ICD via dynamic lookup.
     * This handles surface functions that many ICDs only expose through
     * the loader-facing vkGetInstanceProcAddr, not vk_icdGetInstanceProcAddr. */
dynamic_lookup:
    if (si) {
        /* Try vk_icdGetInstanceProcAddr first */
        PFN_vkVoidFunction fn =
            si->real_get_instance_proc_addr(si->real_instance, name);
        if (fn) return fn;
        /* Many ICDs return NULL from vk_icdGetInstanceProcAddr for physical
         * device functions (surface support, format queries, etc.) and only
         * expose them via vk_icdGetPhysicalDeviceProcAddr. Try that next. */
        PFN_vkGetInstanceProcAddr pdPA = stereo_get_real_pdPA();
        if (pdPA)
            return pdPA(si->real_instance, name);
    }
    return NULL;
}

/* ── ICD negotiation ─────────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion)
{
    if (!pVersion)
        return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t requested = *pVersion;
    /* We support up to interface version 5 */
    if (requested > STEREO_ICD_INTERFACE_VERSION)
        *pVersion = STEREO_ICD_INTERFACE_VERSION;

    STEREO_LOG("Loader requested ICD interface v%u, negotiated v%u",
               requested, *pVersion);
    return VK_SUCCESS;
}

/* ── Primary entry point ─────────────────────────────────────────────────── */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (!pName)
        return NULL;
    return get_instance_proc_addr_internal(instance, pName);
}

/* ── Physical device proc addr (ICD interface v4+) ───────────────────────── */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    /* Physical device commands are dispatched through instance proc addr */
    return get_instance_proc_addr_internal(instance, pName);
}
