/*
 * physdev.c — Physical device wrapper stubs
 *
 * Real VkPhysicalDevice handles from the underlying ICD are returned directly
 * to the Vulkan loader so the loader can properly initialize their dispatch
 * tables (writing its dispatch pointer into handle[0]).  This is required
 * for the NVIDIA driver to accept the handles in surface / swapchain queries.
 *
 * We track the physdev→StereoInstance association in stereo.c via a flat map.
 * LOOKUP_PD_R(pd) looks up _si from that map; _real = pd (the handle IS real).
 */

#include <string.h>
#include <stdlib.h>
#include "stereo_icd.h"

/* Real physdevs are returned directly to the loader — pd IS the real handle.
 * We just need to look up which StereoInstance owns this physdev. */
#define LOOKUP_PD(pd) \
    STEREO_LOG("%s: pd=%p", __func__, (void*)(uintptr_t)(pd)); \
    StereoPhysdev   *_spd  = (StereoPhysdev *)(uintptr_t)(pd); \
    StereoInstance  *_si   = _spd ? _spd->si : NULL; \
    if (!_si) { STEREO_ERR("%s: si==NULL for pd=%p (not our wrapper?)", __func__, (void*)(uintptr_t)(pd)); return; } \
    VkPhysicalDevice _real = _spd->real_pd

#define LOOKUP_PD_R(pd, err) \
    STEREO_LOG("%s: pd=%p", __func__, (void*)(uintptr_t)(pd)); \
    StereoPhysdev   *_spd  = (StereoPhysdev *)(uintptr_t)(pd); \
    StereoInstance  *_si   = _spd ? _spd->si : NULL; \
    if (!_si) { STEREO_ERR("%s: si==NULL for pd=%p (not our wrapper?)", __func__, (void*)(uintptr_t)(pd)); return (err); } \
    VkPhysicalDevice _real = _spd->real_pd

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
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceSupportKHR: pd=%p surface=%p", (void*)pd, (void*)(uintptr_t)surface);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceSupportKHR: _real=%p _si=%p real_inst=%p",
               (void*)_real, (void*)_si, (void*)_si->real_instance);
    if (!_si->real.GetPhysicalDeviceSurfaceSupportKHR) {
        STEREO_ERR("stereo_GetPhysicalDeviceSurfaceSupportKHR: real fn is NULL");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult _r = _si->real.GetPhysicalDeviceSurfaceSupportKHR(
        _real, queueFamilyIndex, surface, pSupported);
    if (_r) STEREO_ERR("stereo_GetPhysicalDeviceSurfaceSupportKHR: real returned %d"
                       " (real_pd=%p surf=%p inst=%p fn=%p)",
                       (int)_r, (void*)_real, (void*)(uintptr_t)surface,
                       (void*)_si->real_instance,
                       (void*)(uintptr_t)(uintptr_t)_si->real.GetPhysicalDeviceSurfaceSupportKHR);
    return _r;
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice           pd,
    VkSurfaceKHR               surface,
    VkSurfaceCapabilitiesKHR  *pCaps)
{
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR: pd=%p surface=%p", (void*)pd, (void*)(uintptr_t)surface);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR: _real=%p real_inst=%p",
               (void*)_real, (void*)_si->real_instance);
    if (!_si->real.GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        STEREO_ERR("stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR: real fn is NULL");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult _r = _si->real.GetPhysicalDeviceSurfaceCapabilitiesKHR(_real, surface, pCaps);
    if (_r) STEREO_ERR("stereo_GetPhysicalDeviceSurfaceCapabilitiesKHR: real returned %d"
                       " (real_pd=%p surf=%p inst=%p)",
                       (int)_r, (void*)_real, (void*)(uintptr_t)surface,
                       (void*)_si->real_instance);
    return _r;
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice    pd,
    VkSurfaceKHR        surface,
    uint32_t           *pCount,
    VkSurfaceFormatKHR *pFormats)
{
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceFormatsKHR: pd=%p surface=%p", (void*)pd, (void*)(uintptr_t)surface);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfaceFormatsKHR) {
        STEREO_ERR("stereo_GetPhysicalDeviceSurfaceFormatsKHR: real fn is NULL");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult _r = _si->real.GetPhysicalDeviceSurfaceFormatsKHR(_real, surface, pCount, pFormats);
    if (_r) STEREO_ERR("stereo_GetPhysicalDeviceSurfaceFormatsKHR: real returned %d", (int)_r);
    return _r;
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice  pd,
    VkSurfaceKHR      surface,
    uint32_t         *pCount,
    VkPresentModeKHR *pModes)
{
    STEREO_LOG("stereo_GetPhysicalDeviceSurfacePresentModesKHR: pd=%p surface=%p", (void*)pd, (void*)(uintptr_t)surface);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    if (!_si->real.GetPhysicalDeviceSurfacePresentModesKHR) {
        STEREO_ERR("stereo_GetPhysicalDeviceSurfacePresentModesKHR: real fn is NULL");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult _r = _si->real.GetPhysicalDeviceSurfacePresentModesKHR(_real, surface, pCount, pModes);
    if (_r) STEREO_ERR("stereo_GetPhysicalDeviceSurfacePresentModesKHR: real returned %d", (int)_r);
    return _r;
}

/* ── Vulkan 1.3 core physdev functions ───────────────────────────────────── */

/* vkGetPhysicalDeviceToolProperties (Vulkan 1.3 core, promoted from EXT).
 * Required by Vulkan loader v7 when the ICD declares api_version >= 1.3.
 * Pass through to the real ICD; if the driver is older than 1.3 and lacks the
 * function, return VK_SUCCESS with zero tools — this is a valid response and
 * the loader accepts it. */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceToolProperties(
    VkPhysicalDevice                    pd,
    uint32_t                           *pToolCount,
    VkPhysicalDeviceToolProperties     *pProperties)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    /* Try 1.3 core name first */
    typedef VkResult (VKAPI_PTR *PFN_t)(VkPhysicalDevice, uint32_t*, VkPhysicalDeviceToolProperties*);
    PFN_t fn = (PFN_t)(uintptr_t)_si->real_get_instance_proc_addr(
        _si->real_instance, "vkGetPhysicalDeviceToolProperties");
    if (!fn)
        fn = (PFN_t)(uintptr_t)_si->real_get_instance_proc_addr(
            _si->real_instance, "vkGetPhysicalDeviceToolPropertiesEXT");
    if (fn)
        return fn(_real, pToolCount, pProperties);
    /* Driver predates 1.3 — no tools active */
    if (pToolCount) *pToolCount = 0;
    return VK_SUCCESS;
}

/* EXT alias — same implementation */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceToolPropertiesEXT(
    VkPhysicalDevice                    pd,
    uint32_t                           *pToolCount,
    VkPhysicalDeviceToolProperties     *pProperties)
{
    return stereo_GetPhysicalDeviceToolProperties(pd, pToolCount, pProperties);
}
