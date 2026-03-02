/*
 * physdev.c — Physical device wrapper stubs
 *
 * VKS3D returns StereoPhysicalDevice* (cast to VkPhysicalDevice) to the
 * loader from vkEnumeratePhysicalDevices.  Every function whose first
 * argument is VkPhysicalDevice must therefore:
 *   1. Look up the real VkPhysicalDevice via stereo_physdev_from_handle()
 *   2. Forward the call to the real ICD using the real handle
 *
 * These are all thin stubs — no stereo logic lives here.  We cannot use
 * PASSTHROUGH_INST / raw real-ICD function pointers for these because the
 * loader would call the real ICD with VKS3D's handle, which the real ICD
 * does not recognise.
 */

#include <string.h>
#include <stdlib.h>
#include "stereo_icd.h"

/* Convenience macro: look up real physdev, bail on unknown handle */
#define LOOKUP_PD(pd) \
    StereoPhysicalDevice *_sp = stereo_physdev_from_handle(pd); \
    if (!_sp) return; \
    StereoInstance *_si = _sp->instance; \
    VkPhysicalDevice _real = _sp->real

#define LOOKUP_PD_R(pd, err) \
    StereoPhysicalDevice *_sp = stereo_physdev_from_handle(pd); \
    if (!_sp) return (err); \
    StereoInstance *_si = _sp->instance; \
    VkPhysicalDevice _real = _sp->real

/* ── Properties ─────────────────────────────────────────────────────────── */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    LOOKUP_PD(pd);
    _si->real.GetPhysicalDeviceProperties(_real, p);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceProperties2)
        _si->real.GetPhysicalDeviceProperties2(_real, p);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFeatures(
    VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    LOOKUP_PD(pd);
    _si->real.GetPhysicalDeviceFeatures(_real, f);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceFeatures2)
        _si->real.GetPhysicalDeviceFeatures2(_real, f);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *p)
{
    LOOKUP_PD(pd);
    _si->real.GetPhysicalDeviceMemoryProperties(_real, p);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceMemoryProperties2)
        _si->real.GetPhysicalDeviceMemoryProperties2(_real, p);
}

/* ── Queue families ──────────────────────────────────────────────────────── */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice pd,
    uint32_t *pCount,
    VkQueueFamilyProperties *pProps)
{
    LOOKUP_PD(pd);
    _si->real.GetPhysicalDeviceQueueFamilyProperties(_real, pCount, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice pd,
    uint32_t *pCount,
    VkQueueFamilyProperties2 *pProps)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceQueueFamilyProperties2)
        _si->real.GetPhysicalDeviceQueueFamilyProperties2(_real, pCount, pProps);
}

/* ── Format / image ──────────────────────────────────────────────────────── */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFormatProperties(
    VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties *p)
{
    LOOKUP_PD(pd);
    _si->real.GetPhysicalDeviceFormatProperties(_real, fmt, p);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceFormatProperties2)
        _si->real.GetPhysicalDeviceFormatProperties2(_real, fmt, p);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice pd, VkFormat fmt, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *p)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    return _si->real.GetPhysicalDeviceImageFormatProperties(
        _real, fmt, type, tiling, usage, flags, p);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice pd, VkFormat fmt, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage,
    VkImageTiling tiling, uint32_t *pCount,
    VkSparseImageFormatProperties *pProps)
{
    LOOKUP_PD(pd);
    _si->real.GetPhysicalDeviceSparseImageFormatProperties(
        _real, fmt, type, samples, usage, tiling, pCount, pProps);
}

/* ── Extension / layer enumeration ──────────────────────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice pd, const char *pLayerName,
    uint32_t *pCount, VkExtensionProperties *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    return _si->real.EnumerateDeviceExtensionProperties(_real, pLayerName, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumerateDeviceLayerProperties(
    VkPhysicalDevice pd, uint32_t *pCount, VkLayerProperties *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    return _si->real.EnumerateDeviceLayerProperties(_real, pCount, pProps);
}

/* ── Surface queries ─────────────────────────────────────────────────────── */
/*
 * These are the functions that were failing with ERROR_INITIALIZATION_FAILED.
 * The real ICD's surface support function was being called with VKS3D's
 * StereoPhysicalDevice* handle instead of the real VkPhysicalDevice.
 */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice pd,
    uint32_t         queueFamilyIndex,
    VkSurfaceKHR     surface,
    VkBool32        *pSupported)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfaceSupportKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfaceSupportKHR(
        _real, queueFamilyIndex, surface, pSupported);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice           pd,
    VkSurfaceKHR               surface,
    VkSurfaceCapabilitiesKHR  *pCaps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfaceCapabilitiesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfaceCapabilitiesKHR(_real, surface, pCaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice    pd,
    VkSurfaceKHR        surface,
    uint32_t           *pCount,
    VkSurfaceFormatKHR *pFormats)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfaceFormatsKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfaceFormatsKHR(_real, surface, pCount, pFormats);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice  pd,
    VkSurfaceKHR      surface,
    uint32_t         *pCount,
    VkPresentModeKHR *pModes)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfacePresentModesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfacePresentModesKHR(_real, surface, pCount, pModes);
}

/* ── Vulkan 1.1 physdev functions (previously missing — caused the crash) ── */
/*
 * These were the root cause of the crash.  icd_main.c's PD_FN table
 * referenced these symbols but they didn't exist, so the linker fell
 * through to dynamic_lookup, returning raw NVIDIA function pointers.
 * The loader then called NVIDIA with our StereoPhysicalDevice* wrapper as
 * the first argument.  NVIDIA wrote into its internal physdev state using
 * the bogus handle, corrupting the heap; the crash manifested a few calls
 * later inside the correctly-wrapped GetPhysicalDeviceProperties.
 */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceImageFormatInfo2 *pInfo,
    VkImageFormatProperties2               *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceImageFormatProperties2)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceImageFormatProperties2(_real, pInfo, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceSparseImageFormatInfo2 *pInfo,
    uint32_t                                     *pCount,
    VkSparseImageFormatProperties2               *pProps)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceSparseImageFormatProperties2)
        _si->real.GetPhysicalDeviceSparseImageFormatProperties2(_real, pInfo, pCount, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceExternalBufferInfo *pInfo,
    VkExternalBufferProperties               *pProps)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceExternalBufferProperties)
        _si->real.GetPhysicalDeviceExternalBufferProperties(_real, pInfo, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceExternalFenceInfo *pInfo,
    VkExternalFenceProperties               *pProps)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceExternalFenceProperties)
        _si->real.GetPhysicalDeviceExternalFenceProperties(_real, pInfo, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceExternalSemaphoreInfo *pInfo,
    VkExternalSemaphoreProperties               *pProps)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceExternalSemaphoreProperties)
        _si->real.GetPhysicalDeviceExternalSemaphoreProperties(_real, pInfo, pProps);
}

/* ── vkEnumeratePhysicalDeviceGroups ─────────────────────────────────────── */
/*
 * Special case: this function takes VkInstance (not VkPhysicalDevice) AND
 * its output contains VkPhysicalDevice handles that must also be translated
 * from real → wrapper before being returned to the loader.
 */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumeratePhysicalDeviceGroups(
    VkInstance                    instance,
    uint32_t                     *pGroupCount,
    VkPhysicalDeviceGroupProperties *pGroupProps)
{
    StereoInstance *si = stereo_instance_from_handle(instance);
    if (!si) return VK_ERROR_INITIALIZATION_FAILED;
    if (!si->real.EnumeratePhysicalDeviceGroups)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkResult res = si->real.EnumeratePhysicalDeviceGroups(
        si->real_instance, pGroupCount, pGroupProps);

    /* Translate each real VkPhysicalDevice handle → our wrapper */
    if (res == VK_SUCCESS && pGroupProps && pGroupCount) {
        for (uint32_t g = 0; g < *pGroupCount; g++) {
            for (uint32_t d = 0; d < pGroupProps[g].physicalDeviceCount; d++) {
                VkPhysicalDevice real_pd = pGroupProps[g].physicalDevices[d];
                StereoPhysicalDevice *sp = stereo_physdev_from_real(real_pd);
                if (sp)
                    pGroupProps[g].physicalDevices[d] = (VkPhysicalDevice)(uintptr_t)sp;
            }
        }
    }
    return res;
}

/* ── KHR surface extensions ──────────────────────────────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    VkSurfaceCapabilities2KHR             *pCaps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfaceCapabilities2KHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfaceCapabilities2KHR(_real, pSurfaceInfo, pCaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    uint32_t                              *pCount,
    VkSurfaceFormat2KHR                   *pFormats)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfaceFormats2KHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfaceFormats2KHR(_real, pSurfaceInfo, pCount, pFormats);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDevicePresentRectanglesKHR(
    VkPhysicalDevice pd,
    VkSurfaceKHR     surface,
    uint32_t        *pRectCount,
    VkRect2D        *pRects)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDevicePresentRectanglesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDevicePresentRectanglesKHR(_real, surface, pRectCount, pRects);
}

/* ── Win32 ───────────────────────────────────────────────────────────────── */

VKAPI_ATTR VkBool32 VKAPI_CALL
stereo_GetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice pd,
    uint32_t         queueFamilyIndex)
{
    LOOKUP_PD_R(pd, VK_FALSE);
    if (!_si->real.GetPhysicalDeviceWin32PresentationSupportKHR)
        return VK_FALSE;
    return _si->real.GetPhysicalDeviceWin32PresentationSupportKHR(_real, queueFamilyIndex);
}

/* ── EXT extensions ──────────────────────────────────────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfacePresentModes2EXT(
    VkPhysicalDevice pd,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    uint32_t        *pCount,
    VkPresentModeKHR *pModes)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfacePresentModes2EXT)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSurfacePresentModes2EXT(_real, pSurfaceInfo, pCount, pModes);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceCalibrateableTimeDomainsEXT(
    VkPhysicalDevice pd,
    uint32_t        *pCount,
    VkTimeDomainEXT *pDomains)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceCalibrateableTimeDomainsEXT)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceCalibrateableTimeDomainsEXT(_real, pCount, pDomains);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMultisamplePropertiesEXT(
    VkPhysicalDevice pd,
    VkSampleCountFlagBits samples,
    VkMultisamplePropertiesEXT *pProps)
{
    LOOKUP_PD(pd);
    if (_si->real.GetPhysicalDeviceMultisamplePropertiesEXT)
        _si->real.GetPhysicalDeviceMultisamplePropertiesEXT(_real, samples, pProps);
}

/* ── NV extensions ───────────────────────────────────────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceExternalImageFormatPropertiesNV(
    VkPhysicalDevice pd,
    VkFormat fmt, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkExternalMemoryHandleTypeFlagsNV externalHandleType,
    VkExternalImageFormatPropertiesNV *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceExternalImageFormatPropertiesNV)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceExternalImageFormatPropertiesNV(
        _real, fmt, type, tiling, usage, flags, externalHandleType, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceCooperativeMatrixPropertiesNV(
    VkPhysicalDevice pd,
    uint32_t        *pCount,
    VkCooperativeMatrixPropertiesNV *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceCooperativeMatrixPropertiesNV)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceCooperativeMatrixPropertiesNV(_real, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(
    VkPhysicalDevice pd,
    uint32_t        *pCount,
    VkFramebufferMixedSamplesCombinationNV *pCombinations)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return _si->real.GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(
        _real, pCount, pCombinations);
}

/* NVX: stored as PFN_vkVoidFunction because struct types were removed from
 * newer SDK versions.  Cast to a raw function pointer at call site.        */
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceGeneratedCommandsPropertiesNVX(
    VkPhysicalDevice pd, void *pFeatures, void *pLimits)
{
    LOOKUP_PD(pd);
    typedef void (VKAPI_PTR *PFN_t)(VkPhysicalDevice, void *, void *);
    PFN_t fn = (PFN_t)_si->real.GetPhysicalDeviceGeneratedCommandsPropertiesNVX;
    if (fn) fn(_real, pFeatures, pLimits);
}
