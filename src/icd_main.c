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
stereo_dl_t               stereo_get_real_icd_handle(void);

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
 * the loader calls it with the real physdev handle — this is fine because
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
    /* Vulkan 1.3 core physdev */
    PD_FN(GetPhysicalDeviceToolProperties)
    PD_FN(GetPhysicalDeviceToolPropertiesEXT)
    PD_FN(GetPhysicalDeviceSurfaceCapabilities2KHR)
    PD_FN(GetPhysicalDeviceSurfaceFormats2KHR)
    PD_FN(GetPhysicalDevicePresentRectanglesKHR)
    PD_FN(GetPhysicalDeviceDisplayPropertiesKHR)
    PD_FN(GetPhysicalDeviceDisplayPlanePropertiesKHR)
    PD_FN(GetDisplayPlaneSupportedDisplaysKHR)
    PD_FN(GetDisplayModePropertiesKHR)
    PD_FN(CreateDisplayModeKHR)
    PD_FN(GetDisplayPlaneCapabilitiesKHR)
    PD_FN(GetPhysicalDeviceDisplayProperties2KHR)
    PD_FN(GetPhysicalDeviceDisplayPlaneProperties2KHR)
    PD_FN(GetDisplayModeProperties2KHR)
    PD_FN(GetDisplayPlaneCapabilities2KHR)
    PD_FN(GetPhysicalDeviceSurfaceCapabilities2EXT)
    /* ── Win32 ── */
#ifdef VK_KHR_win32_surface
    PD_FN(GetPhysicalDeviceWin32PresentationSupportKHR)
#endif
    /* ── EXT extensions ── */
#ifdef VK_EXT_full_screen_exclusive
    PD_FN(GetPhysicalDeviceSurfacePresentModes2EXT)
#endif
#ifdef VK_EXT_calibrated_timestamps
    PD_FN(GetPhysicalDeviceCalibrateableTimeDomainsEXT)
#endif
    PD_FN(GetPhysicalDeviceMultisamplePropertiesEXT)
    /* ── NV / NVX vendor extensions ── */
    PD_FN(GetPhysicalDeviceExternalImageFormatPropertiesNV)
    PD_FN(GetPhysicalDeviceGeneratedCommandsPropertiesNVX)
#ifdef VK_NV_cooperative_matrix
    PD_FN(GetPhysicalDeviceCooperativeMatrixPropertiesNV)
#endif
#ifdef VK_NV_coverage_reduction_mode
    PD_FN(GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)
#endif
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
#ifdef VK_KHR_win32_surface
    if (!strcmp(name, "vkCreateWin32SurfaceKHR"))
        return (PFN_vkVoidFunction)stereo_CreateWin32SurfaceKHR;
#endif
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
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)stereo_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)stereo_DestroyDevice;
    if (!strcmp(name, "vkCreateImageView"))
        return (PFN_vkVoidFunction)stereo_CreateImageView;
    if (!strcmp(name, "vkCreateRenderPass"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass;
#ifdef VK_KHR_create_renderpass2
    if (!strcmp(name, "vkCreateRenderPass2KHR"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass2KHR;
#endif
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

    /* Everything else: forward instance-level functions dynamically. */
    if (si && name) {
        if (strncmp(name, "vkGetPhysicalDevice",           19) == 0 ||
            strncmp(name, "vkEnumerateDevice",             17) == 0 ||
            strncmp(name, "vkEnumeratePhysicalDeviceGroup", 30) == 0) {
            /* Forward unknown physdev functions via generic trampoline.
             * The trampoline translates our StereoPhysdev* to real_pd
             * before calling the real ICD function pointer. */
            extern PFN_vkVoidFunction stereo_physdev_trampoline_lookup(
                StereoInstance *si, const char *name);
            PFN_vkVoidFunction t = stereo_physdev_trampoline_lookup(si, name);
            if (t) return t;
            /* Fall through to instance-level lookup below */
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


/* ── Device-level proc addr ─────────────────────────────────────────────────
 *
 * The Vulkan loader calls vkGetDeviceProcAddr(device, name) to build the
 * device dispatch table.  If we don't intercept this, the loader gets
 * NVIDIA's real vkGetDeviceProcAddr which returns NVIDIA's own function
 * pointers — bypassing ALL of VKS3D's device-level wrappers (swapchain,
 * present, render pass injection).  This is why stereo was not working.
 *
 * We must intercept every device-level command VKS3D wraps, then forward
 * everything else to the real device.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
stereo_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    if (!pName) return NULL;

    /* ── VKS3D-wrapped device commands ───────────────────────────────── */
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)stereo_GetDeviceProcAddr;
    if (!strcmp(pName, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)stereo_DestroyDevice;
    if (!strcmp(pName, "vkCreateImage"))
        return (PFN_vkVoidFunction)stereo_CreateImage;
    if (!strcmp(pName, "vkCreateImageView"))
        return (PFN_vkVoidFunction)stereo_CreateImageView;
    if (!strcmp(pName, "vkCreateRenderPass"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass;
#ifdef VK_KHR_create_renderpass2
    if (!strcmp(pName, "vkCreateRenderPass2KHR"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass2KHR;
    if (!strcmp(pName, "vkCreateRenderPass2"))
        return (PFN_vkVoidFunction)stereo_CreateRenderPass2KHR;
#endif
    if (!strcmp(pName, "vkCreateShaderModule"))
        return (PFN_vkVoidFunction)stereo_CreateShaderModule;
    if (!strcmp(pName, "vkDestroyShaderModule"))
        return (PFN_vkVoidFunction)stereo_DestroyShaderModule;
    if (!strcmp(pName, "vkCreateSwapchainKHR"))
        return (PFN_vkVoidFunction)stereo_CreateSwapchainKHR;
    if (!strcmp(pName, "vkDestroySwapchainKHR"))
        return (PFN_vkVoidFunction)stereo_DestroySwapchainKHR;
    if (!strcmp(pName, "vkGetSwapchainImagesKHR"))
        return (PFN_vkVoidFunction)stereo_GetSwapchainImagesKHR;
    if (!strcmp(pName, "vkAcquireNextImageKHR"))
        return (PFN_vkVoidFunction)stereo_AcquireNextImageKHR;
    if (!strcmp(pName, "vkQueuePresentKHR"))
        return (PFN_vkVoidFunction)stereo_QueuePresentKHR;

    /* ── Forward everything else to the real device ──────────────────── */
    /* Look up the real device from our registry to get its proc addr fn */
    extern StereoDevice g_devices[];
    extern uint32_t     g_device_count;
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].real_device == device ||
            (VkDevice)(uintptr_t)&g_devices[i] == device) {
            PFN_vkGetDeviceProcAddr real_gdpa =
                (PFN_vkGetDeviceProcAddr)
                g_devices[i].real.GetDeviceProcAddr;
            if (real_gdpa)
                return real_gdpa(g_devices[i].real_device, pName);
            break;
        }
    }
    /* Fallback: use instance-level lookup */
    return get_instance_proc_addr_internal(VK_NULL_HANDLE, pName);
}

/* ── Loader interface v6+: DXGI adapter physdev enumeration ─────────────────
 *
 * At loader interface version 6+, the Vulkan loader uses DXGI adapter
 * enumeration alongside the ICD registry.  For each DXGI adapter it calls
 * vk_icdEnumerateAdapterPhysicalDevices on every registered ICD.
 *
 * If we don't implement this, the loader falls back to the old
 * vkEnumeratePhysicalDevices path for VKS3D but ALSO loads nvoglv64 via
 * DXGI (if nvoglv64 implements interface v6+), presenting the app with BOTH
 * VKS3D's and nvoglv64's physdevs.  The app then picks nvoglv64's physdev
 * for rendering, completely bypassing stereo_CreateDevice.
 *
 * Fix: implement the function, forward to the real ICD, and wrap the
 * returned physdevs so the app only ever sees StereoPhysdev* handles.      */
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdEnumerateAdapterPhysicalDevices(
    VkInstance  instance,
    LUID        adapterLUID,
    uint32_t   *pPhysicalDeviceCount,
    VkPhysicalDevice *pPhysicalDevices)
{
    STEREO_LOG("vk_icdEnumerateAdapterPhysicalDevices: instance=%p LUID=%08lx:%08lx",
               (void*)instance,
               (unsigned long)adapterLUID.HighPart,
               (unsigned long)adapterLUID.LowPart);

    StereoInstance *si = stereo_instance_from_handle(instance);
    if (!si) {
        STEREO_ERR("vk_icdEnumerateAdapterPhysicalDevices: unknown instance %p", (void*)instance);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Look up real ICD's adapter enumeration function via GetProcAddress.
     * It's an ICD entry point, not a Vulkan API function, so it's not in
     * the vkGetInstanceProcAddr table. */
    typedef VkResult (VKAPI_PTR *PFN_EnumAdapter)(
        VkInstance, LUID, uint32_t *, VkPhysicalDevice *);
    stereo_dl_t real_mod = stereo_get_real_icd_handle();
    PFN_EnumAdapter real_fn = real_mod
        ? (PFN_EnumAdapter)GetProcAddress((HMODULE)real_mod,
                                           "vk_icdEnumerateAdapterPhysicalDevices")
        : NULL;

    if (!real_fn) {
        /* Real ICD doesn't implement it — fall back to standard enumeration */
        STEREO_LOG("vk_icdEnumerateAdapterPhysicalDevices: real ICD has no adapter fn, "
                   "falling back to vkEnumeratePhysicalDevices");
        return si->real.EnumeratePhysicalDevices
            ? (VkResult)si->real.EnumeratePhysicalDevices(
                  si->real_instance, pPhysicalDeviceCount, pPhysicalDevices)
            : VK_ERROR_INITIALIZATION_FAILED;
    }

    /* Two-call pattern: first get count, then fill array */
    if (!pPhysicalDevices) {
        VkResult r = real_fn(si->real_instance, adapterLUID, pPhysicalDeviceCount, NULL);
        STEREO_LOG("vk_icdEnumerateAdapterPhysicalDevices: count=%u", *pPhysicalDeviceCount);
        return r;
    }

    /* Get raw physdevs from real ICD then wrap each one */
    uint32_t count = *pPhysicalDeviceCount;
    VkPhysicalDevice raw[MAX_PHYSICAL_DEVICES];
    if (count > MAX_PHYSICAL_DEVICES) count = MAX_PHYSICAL_DEVICES;
    VkResult r = real_fn(si->real_instance, adapterLUID, &count, raw);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;

    for (uint32_t i = 0; i < count; i++) {
        StereoPhysdev *spd = stereo_physdev_get_or_create(raw[i], si);
        pPhysicalDevices[i] = spd ? (VkPhysicalDevice)(uintptr_t)spd : raw[i];
        STEREO_LOG("vk_icdEnumerateAdapterPhysicalDevices: [%u] real=%p wrapper=%p",
                   i, (void*)raw[i], (void*)pPhysicalDevices[i]);
    }
    *pPhysicalDeviceCount = count;
    return r;
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

/* ── Public export: vkGetDeviceProcAddr ─────────────────────────────────────
 * Exported from the DLL so the loader can find it.  The loader queries this
 * via GetProcAddress("vkGetDeviceProcAddr") or vk_icdGetInstanceProcAddr.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    return stereo_GetDeviceProcAddr(device, pName);
}

/* ── Named DLL exports for device-level intercept functions ─────────────────
 * The loader uses vk_icdGetInstanceProcAddr to get these, but exporting them
 * as named symbols allows tools (vks3d_diag, validation layers, debuggers)
 * to verify presence via GetProcAddress, and provides a fallback for loaders
 * that call GetProcAddress after failing vk_icdGetInstanceProcAddr.
 *
 * Each function simply forwards to the internal stereo_* implementation.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *c,
                     const VkAllocationCallbacks *a, VkSwapchainKHR *s)
{ return stereo_CreateSwapchainKHR(d, c, a, s); }

VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a)
{ stereo_DestroySwapchainKHR(d, s, a); }

VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *c, VkImage *i)
{ return stereo_GetSwapchainImagesKHR(d, s, c, i); }

VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t,
                      VkSemaphore sem, VkFence f, uint32_t *i)
{ return stereo_AcquireNextImageKHR(d, s, t, sem, f, i); }

VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *p)
{ return stereo_QueuePresentKHR(q, p); }

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *c,
                   const VkAllocationCallbacks *a, VkRenderPass *r)
{ return stereo_CreateRenderPass(d, c, a, r); }

#ifdef VK_KHR_create_renderpass2
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass2KHR(VkDevice d, const VkRenderPassCreateInfo2 *c,
                       const VkAllocationCallbacks *a, VkRenderPass *r)
{ return stereo_CreateRenderPass2KHR(d, c, a, r); }
#endif

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *c,
                     const VkAllocationCallbacks *a, VkShaderModule *s)
{ return stereo_CreateShaderModule(d, c, a, s); }

VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice d, VkShaderModule s, const VkAllocationCallbacks *a)
{ stereo_DestroyShaderModule(d, s, a); }

/* Named export: vkCreateImageView */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *c,
                  const VkAllocationCallbacks *a, VkImageView *v)
{ return stereo_CreateImageView(d, c, a, v); }
