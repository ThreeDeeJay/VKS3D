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
     * Physical device functions — return our own wrapper stubs (defined at the
     * bottom of this file), NOT the real ICD's raw function pointers.
     *
     * We must not return the real ICD's pointer here: the loader will call it
     * with VKS3D's StereoPhysicalDevice* handle (which is what we returned
     * from vkEnumeratePhysicalDevices), not the real ICD's VkPhysicalDevice.
     * The real ICD does not recognise our handle and returns errors.
     * Our wrappers look up the real handle via stereo_physdev_from_handle()
     * before forwarding to the real ICD.
     */
/* PD_FN: return our wrapper stub for any function whose first argument is
 * VkPhysicalDevice.  We MUST NOT return the real ICD's raw pointer here:
 * the loader calls it with our StereoPhysicalDevice* wrapper handle, not
 * the real ICD's VkPhysicalDevice.  The real ICD would dereference our
 * wrapper as its own internal struct, corrupt heap state, and crash.      */
#define PD_FN(fn) if (!strcmp(name, "vk"#fn)) return (PFN_vkVoidFunction)stereo_##fn;

    /* ── Vulkan 1.0 core ── */
    PD_FN(GetPhysicalDeviceProperties)
    PD_FN(GetPhysicalDeviceFeatures)
    PD_FN(GetPhysicalDeviceMemoryProperties)
    PD_FN(GetPhysicalDeviceQueueFamilyProperties)
    PD_FN(GetPhysicalDeviceFormatProperties)
    PD_FN(GetPhysicalDeviceImageFormatProperties)
    PD_FN(GetPhysicalDeviceSparseImageFormatProperties)
    PD_FN(EnumerateDeviceExtensionProperties)
    PD_FN(EnumerateDeviceLayerProperties)
    /* ── Vulkan 1.1 core (promoted from KHR) ── */
    PD_FN(GetPhysicalDeviceProperties2)
    PD_FN(GetPhysicalDeviceFeatures2)
    PD_FN(GetPhysicalDeviceMemoryProperties2)
    PD_FN(GetPhysicalDeviceQueueFamilyProperties2)
    PD_FN(GetPhysicalDeviceFormatProperties2)
    PD_FN(GetPhysicalDeviceImageFormatProperties2)
    PD_FN(GetPhysicalDeviceSparseImageFormatProperties2)
    PD_FN(GetPhysicalDeviceExternalBufferProperties)
    PD_FN(GetPhysicalDeviceExternalFenceProperties)
    PD_FN(GetPhysicalDeviceExternalSemaphoreProperties)
    /* ── VkPhysicalDeviceGroup enumeration (handles inside need wrapping) ── */
    PD_FN(EnumeratePhysicalDeviceGroups)
    /* ── Surface KHR ── */
    PD_FN(GetPhysicalDeviceSurfaceSupportKHR)
    PD_FN(GetPhysicalDeviceSurfaceCapabilitiesKHR)
    PD_FN(GetPhysicalDeviceSurfaceFormatsKHR)
    PD_FN(GetPhysicalDeviceSurfacePresentModesKHR)
    PD_FN(GetPhysicalDeviceSurfaceCapabilities2KHR)
    PD_FN(GetPhysicalDeviceSurfaceFormats2KHR)
    PD_FN(GetPhysicalDevicePresentRectanglesKHR)
    /* ── Win32 ── */
    PD_FN(GetPhysicalDeviceWin32PresentationSupportKHR)
    /* ── EXT extensions ── */
    PD_FN(GetPhysicalDeviceSurfacePresentModes2EXT)
    PD_FN(GetPhysicalDeviceCalibrateableTimeDomainsEXT)
    PD_FN(GetPhysicalDeviceMultisamplePropertiesEXT)
    /* ── NV / NVX vendor extensions ── */
    PD_FN(GetPhysicalDeviceExternalImageFormatPropertiesNV)
    PD_FN(GetPhysicalDeviceGeneratedCommandsPropertiesNVX)
    PD_FN(GetPhysicalDeviceCooperativeMatrixPropertiesNV)
    PD_FN(GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)
#undef PD_FN

    /* ── KHR aliases — same underlying wrapper as the core 1.1 function ──── */
    /* Vulkan 1.1 promoted these KHR extensions to core.  The NVIDIA ICD uses
     * the same function pointer for both names.  We alias to the core wrapper
     * rather than duplicating stubs. */
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceMemoryProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceQueueFamilyProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceFormatProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceImageFormatProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceSparseImageFormatProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceExternalBufferPropertiesKHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceExternalBufferProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceExternalFencePropertiesKHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceExternalFenceProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"))
        return (PFN_vkVoidFunction)stereo_GetPhysicalDeviceExternalSemaphoreProperties;
    if (!strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
        return (PFN_vkVoidFunction)stereo_EnumeratePhysicalDeviceGroups;

    /* ── Instance-level surface / debug ─────────────────────────────────── */
    /* These take VkInstance as first arg.  We MUST wrap them so the real ICD
     * receives si->real_instance, not our StereoInstance* wrapper. */
    if (!strcmp(name, "vkDestroySurfaceKHR"))
        return (PFN_vkVoidFunction)stereo_DestroySurfaceKHR;
    if (!strcmp(name, "vkCreateWin32SurfaceKHR"))
        return (PFN_vkVoidFunction)stereo_CreateWin32SurfaceKHR;
    if (!strcmp(name, "vkCreateDebugReportCallbackEXT"))
        return (PFN_vkVoidFunction)stereo_CreateDebugReportCallbackEXT;
    if (!strcmp(name, "vkDestroyDebugReportCallbackEXT"))
        return (PFN_vkVoidFunction)stereo_DestroyDebugReportCallbackEXT;
    if (!strcmp(name, "vkDebugReportMessageEXT"))
        return (PFN_vkVoidFunction)stereo_DebugReportMessageEXT;
    if (!strcmp(name, "vkCreateDebugUtilsMessengerEXT"))
        return (PFN_vkVoidFunction)stereo_CreateDebugUtilsMessengerEXT;
    if (!strcmp(name, "vkDestroyDebugUtilsMessengerEXT"))
        return (PFN_vkVoidFunction)stereo_DestroyDebugUtilsMessengerEXT;
    if (!strcmp(name, "vkSubmitDebugUtilsMessageEXT"))
        return (PFN_vkVoidFunction)stereo_SubmitDebugUtilsMessageEXT;

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

    /* Everything else: forward instance-level functions dynamically.
     *
     * SAFETY GUARD: if any vkGetPhysicalDevice* or vkEnumerateDevice* function
     * slipped through the PD_FN table above, we must NOT forward it to the
     * real ICD — that would leak a raw pointer that gets called with our
     * StereoPhysicalDevice* wrapper handle, corrupting the real ICD's state.
     * Return NULL instead; the loader will report the function as unsupported. */
dynamic_lookup:
    if (si && name) {
        if (strncmp(name, "vkGetPhysicalDevice",           19) == 0 ||
            strncmp(name, "vkEnumerateDevice",             17) == 0 ||
            strncmp(name, "vkEnumeratePhysicalDeviceGroup", 30) == 0) {
            STEREO_LOG("dynamic_lookup: BLOCKED unhandled physdev fn '%s' "
                       "(returning NULL — add a wrapper stub in physdev_ext.c)", name);
            return NULL;
        }
        PFN_vkVoidFunction fn =
            si->real_get_instance_proc_addr(si->real_instance, name);
        if (fn) return fn;
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
    STEREO_LOG("vk_icdNegotiateLoaderICDInterfaceVersion: called, pVersion=%p",
               (void*)pVersion);
    if (!pVersion) {
        STEREO_ERR("vk_icdNegotiateLoaderICDInterfaceVersion: pVersion is NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint32_t requested = *pVersion;
    if (requested > STEREO_ICD_INTERFACE_VERSION)
        *pVersion = STEREO_ICD_INTERFACE_VERSION;

    STEREO_LOG("vk_icdNegotiateLoaderICDInterfaceVersion: requested=%u negotiated=%u",
               requested, *pVersion);
    return VK_SUCCESS;
}

/* ── Primary entry point ─────────────────────────────────────────────────── */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (!pName)
        return NULL;
    STEREO_LOG("vk_icdGetInstanceProcAddr: instance=%p name='%s'",
               (void*)instance, pName);
    PFN_vkVoidFunction fn = get_instance_proc_addr_internal(instance, pName);
    STEREO_LOG("vk_icdGetInstanceProcAddr: '%s' -> %p", pName, (void*)(uintptr_t)fn);
    return fn;
}

/* ── Physical device proc addr (ICD interface v4+) ───────────────────────── */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    if (!pName)
        return NULL;
    STEREO_LOG("vk_icdGetPhysicalDeviceProcAddr: instance=%p name='%s'",
               (void*)instance, pName);
    PFN_vkVoidFunction fn = get_instance_proc_addr_internal(instance, pName);
    STEREO_LOG("vk_icdGetPhysicalDeviceProcAddr: '%s' -> %p", pName, (void*)(uintptr_t)fn);
    return fn;
}
