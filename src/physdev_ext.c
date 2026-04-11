/*
 * physdev_ext.c — Extension physical device wrapper stubs
 *
 * Every vkGetPhysicalDevice* / vkEnumerateDevice* / vkEnumeratePhysicalDeviceGroups
 * function that is NOT in physdev.c (Vulkan 1.0/1.1 core) lives here.
 *
 * WHY these wrappers are mandatory
 * ─────────────────────────────────
 * VKS3D returns StereoPhysicalDevice* (cast to VkPhysicalDevice) from
 * vkEnumeratePhysicalDevices.  If dynamic_lookup returned the real ICD's
 * raw function pointer for any physdev-level function, the Vulkan loader
 * would call it with our StereoPhysicalDevice* wrapper handle.  NVIDIA
 * would dereference that as its own internal struct, corrupt heap state,
 * and crash a few calls later.  The fix: every such function must go
 * through a stub that translates the wrapper handle back to the real
 * VkPhysicalDevice before forwarding to the real ICD.
 *
 * Functions observed via VKS3D.log analysis (NVIDIA driver, this system):
 *   vkGetPhysicalDeviceFeatures2KHR                   → 51C7F9F0 (alias)
 *   vkGetPhysicalDeviceProperties2KHR                 → 51C7FB50 (alias)
 *   vkGetPhysicalDeviceFormatProperties2KHR           → 51C7FA30 (alias)
 *   vkGetPhysicalDeviceImageFormatProperties2{,KHR}   → 51C7FA90
 *   vkGetPhysicalDeviceSparseImageFormatProperties2{,KHR} → 51C7FBD0
 *   vkGetPhysicalDeviceMemoryProperties2KHR           → 51C7FAD0 (alias)
 *   vkGetPhysicalDeviceQueueFamilyProperties2KHR      → 51C7FB90 (alias)
 *   vkGetPhysicalDeviceExternalBufferProperties{,KHR} → 51C7F950
 *   vkGetPhysicalDeviceExternalFenceProperties{,KHR}  → 51C7F970
 *   vkGetPhysicalDeviceExternalSemaphoreProperties{,KHR} → 51C7F9B0
 *   vkEnumeratePhysicalDeviceGroups{,KHR}             → 51C7F4B0
 *   vkGetPhysicalDeviceSurfaceCapabilities2KHR        → 51C7FC10
 *   vkGetPhysicalDeviceSurfaceFormats2KHR             → 51C7FC50
 *   vkGetPhysicalDeviceSurfacePresentModes2EXT        → 51C7FC90
 *   vkGetPhysicalDevicePresentRectanglesKHR           → 51C7FB10
 *   vkGetPhysicalDeviceWin32PresentationSupportKHR    → 51C7FCF0
 *   vkGetPhysicalDeviceCalibrateableTimeDomainsEXT    → 51C7F910
 *   vkGetPhysicalDeviceMultisamplePropertiesEXT       → 51C7FAF0
 *   vkGetPhysicalDeviceExternalImageFormatPropertiesNV → 51C7F990
 *   vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX → 51C7FA50
 *   vkGetPhysicalDeviceCooperativeMatrixPropertiesNV  → 51C7F930
 *   vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV → 51C7FBF0
 */

#include <string.h>
#include <stdlib.h>
#include "stereo_icd.h"

/* Look up an extension fn via giPA then pdPA — never returns wrong-handle raw ptr */
/* Look up an extension fn via pdPA first (raw ICD physdev-level impl),
 * falling back to giPA.  For physdev-level surface/WSI functions on NVIDIA,
 * giPA returns a loader-entry trampoline that fails when called directly;
 * pdPA returns the actual implementation meant for direct ICD-to-ICD calls. */
static inline PFN_vkVoidFunction ext_fn(StereoInstance *si, const char *name)
{
    PFN_vkGetInstanceProcAddr pdPA = stereo_get_real_pdPA();
    if (pdPA) {
        PFN_vkVoidFunction fn = (PFN_vkVoidFunction)pdPA(si->real_instance, name);
        if (fn) return fn;
    }
    return (PFN_vkVoidFunction)si->real_get_instance_proc_addr(si->real_instance, name);
}

/* Real physdevs passed through to loader — pd IS the real handle */
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

/* ── Vulkan 1.1 KHR aliases (same ABI as core — just delegate) ──────────── */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
    { stereo_GetPhysicalDeviceFeatures2(pd, f); }

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
    { stereo_GetPhysicalDeviceProperties2(pd, p); }

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p)
    { stereo_GetPhysicalDeviceFormatProperties2(pd, fmt, p); }

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
    { stereo_GetPhysicalDeviceMemoryProperties2(pd, p); }

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceQueueFamilyProperties2KHR(
    VkPhysicalDevice pd, uint32_t *pCount, VkQueueFamilyProperties2 *pProps)
    { stereo_GetPhysicalDeviceQueueFamilyProperties2(pd, pCount, pProps); }

/* ── ImageFormatProperties2 (core 1.1 + KHR alias) ─────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice                        pd,
    const VkPhysicalDeviceImageFormatInfo2 *pInfo,
    VkImageFormatProperties2               *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pInfo, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceImageFormatProperties2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *pInfo,
    VkImageFormatProperties2 *pProps)
    { return stereo_GetPhysicalDeviceImageFormatProperties2(pd, pInfo, pProps); }

/* ── SparseImageFormatProperties2 (core 1.1 + KHR alias) ───────────────── */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice                              pd,
    const VkPhysicalDeviceSparseImageFormatInfo2 *pInfo,
    uint32_t                                     *pCount,
    VkSparseImageFormatProperties2               *pProps)
{
    LOOKUP_PD(pd);
    typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceSparseImageFormatInfo2 *,
        uint32_t *, VkSparseImageFormatProperties2 *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSparseImageFormatProperties2");
    if (fn) fn(_real, pInfo, pCount, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *pInfo,
    uint32_t *pCount, VkSparseImageFormatProperties2 *pProps)
    { stereo_GetPhysicalDeviceSparseImageFormatProperties2(pd, pInfo, pCount, pProps); }

/* ── External resource properties (core 1.1 + KHR aliases) ─────────────── */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice                          pd,
    const VkPhysicalDeviceExternalBufferInfo *pInfo,
    VkExternalBufferProperties               *pProps)
{
    LOOKUP_PD(pd);
    typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceExternalBufferInfo *, VkExternalBufferProperties *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceExternalBufferProperties");
    if (fn) fn(_real, pInfo, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalBufferPropertiesKHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *pInfo,
    VkExternalBufferProperties *pProps)
    { stereo_GetPhysicalDeviceExternalBufferProperties(pd, pInfo, pProps); }

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice                         pd,
    const VkPhysicalDeviceExternalFenceInfo *pInfo,
    VkExternalFenceProperties               *pProps)
{
    LOOKUP_PD(pd);
    typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceExternalFenceInfo *, VkExternalFenceProperties *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceExternalFenceProperties");
    if (fn) fn(_real, pInfo, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalFencePropertiesKHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfo *pInfo,
    VkExternalFenceProperties *pProps)
    { stereo_GetPhysicalDeviceExternalFenceProperties(pd, pInfo, pProps); }

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice                             pd,
    const VkPhysicalDeviceExternalSemaphoreInfo *pInfo,
    VkExternalSemaphoreProperties               *pProps)
{
    LOOKUP_PD(pd);
    typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceExternalSemaphoreInfo *, VkExternalSemaphoreProperties *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (fn) fn(_real, pInfo, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalSemaphorePropertiesKHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *pInfo,
    VkExternalSemaphoreProperties *pProps)
    { stereo_GetPhysicalDeviceExternalSemaphoreProperties(pd, pInfo, pProps); }

/* ── EnumeratePhysicalDeviceGroups (instance-level but returns pd handles) ─
 * Must wrap: the VkPhysicalDeviceGroupProperties structs it fills contain
 * VkPhysicalDevice handles that must be replaced with our wrapper pointers. */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumeratePhysicalDeviceGroups(
    VkInstance                       instance,
    uint32_t                        *pCount,
    VkPhysicalDeviceGroupProperties *pProps)
{
    StereoInstance *si = stereo_instance_from_handle(instance);
    if (!si) return VK_ERROR_INITIALIZATION_FAILED;
    typedef VkResult (VKAPI_PTR *PFN)(VkInstance, uint32_t *,
                                      VkPhysicalDeviceGroupProperties *);
    PFN fn = (PFN)ext_fn(si, "vkEnumeratePhysicalDeviceGroups");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkResult res = fn(si->real_instance, pCount, pProps);

    /* Wrap any physdevs found in the groups (same as stereo_EnumeratePhysicalDevices) */
    if (res == VK_SUCCESS && pProps) {
        for (uint32_t g = 0; g < *pCount; g++) {
            VkPhysicalDeviceGroupProperties *grp = &pProps[g];
            for (uint32_t p = 0; p < grp->physicalDeviceCount; p++) {
                VkPhysicalDevice real_pd = grp->physicalDevices[p];
                StereoPhysdev *spd = stereo_physdev_get_or_create(real_pd, si);
                if (spd)
                    grp->physicalDevices[p] = (VkPhysicalDevice)(uintptr_t)spd;
            }
        }
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumeratePhysicalDeviceGroupsKHR(
    VkInstance instance, uint32_t *pCount, VkPhysicalDeviceGroupProperties *pProps)
    { return stereo_EnumeratePhysicalDeviceGroups(instance, pCount, pProps); }

/* ── Surface extension queries ───────────────────────────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice                      pd,
    const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
    VkSurfaceCapabilities2KHR             *pCaps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR *, VkSurfaceCapabilities2KHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pInfo, pCaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice                      pd,
    const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
    uint32_t                              *pCount,
    VkSurfaceFormat2KHR                   *pFormats)
{
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceFormats2KHR: pd=%p", (void*)pd);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR *, uint32_t *, VkSurfaceFormat2KHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSurfaceFormats2KHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pInfo, pCount, pFormats);
}

#ifdef VK_EXT_full_screen_exclusive
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfacePresentModes2EXT(
    VkPhysicalDevice                      pd,
    const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
    uint32_t                              *pCount,
    VkPresentModeKHR                      *pModes)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR *, uint32_t *, VkPresentModeKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSurfacePresentModes2EXT");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pInfo, pCount, pModes);
}
#endif /* VK_EXT_full_screen_exclusive */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDevicePresentRectanglesKHR(
    VkPhysicalDevice pd,
    VkSurfaceKHR     surface,
    uint32_t        *pRectCount,
    VkRect2D        *pRects)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
        VkSurfaceKHR, uint32_t *, VkRect2D *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDevicePresentRectanglesKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, surface, pRectCount, pRects);
}

/* ── Win32 ───────────────────────────────────────────────────────────────── */

VKAPI_ATTR VkBool32 VKAPI_CALL
stereo_GetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice pd, uint32_t queueFamilyIndex)
{
    LOOKUP_PD_R(pd, VK_FALSE);
    typedef VkBool32 (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    if (!fn) return VK_FALSE;
    return fn(_real, queueFamilyIndex);
}

/* ── EXT extensions ──────────────────────────────────────────────────────── */

#ifdef VK_EXT_calibrated_timestamps
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceCalibrateableTimeDomainsEXT(
    VkPhysicalDevice pd, uint32_t *pCount, VkTimeDomainEXT *pDomains)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *, VkTimeDomainEXT *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pDomains);
}
#endif /* VK_EXT_calibrated_timestamps */

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMultisamplePropertiesEXT(
    VkPhysicalDevice            pd,
    VkSampleCountFlagBits       samples,
    VkMultisamplePropertiesEXT *pProps)
{
    LOOKUP_PD(pd);
    typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice,
        VkSampleCountFlagBits, VkMultisamplePropertiesEXT *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceMultisamplePropertiesEXT");
    if (fn) fn(_real, samples, pProps);
}

/* ── NV / NVX vendor extensions ─────────────────────────────────────────── */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceExternalImageFormatPropertiesNV(
    VkPhysicalDevice                   pd,
    VkFormat                           format,
    VkImageType                        type,
    VkImageTiling                      tiling,
    VkImageUsageFlags                  usage,
    VkImageCreateFlags                 flags,
    VkExternalMemoryHandleTypeFlagsNV  externalHandleType,
    VkExternalImageFormatPropertiesNV *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV fn =
        (PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV)
        ext_fn(_si, "vkGetPhysicalDeviceExternalImageFormatPropertiesNV");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, format, type, tiling, usage, flags, externalHandleType, pProps);
}

VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceGeneratedCommandsPropertiesNVX(
    VkPhysicalDevice pd,
    void            *pFeatures,   /* VkDeviceGeneratedCommandsFeaturesNVX* */
    void            *pLimits)     /* VkDeviceGeneratedCommandsLimitsNVX*   */
{
    LOOKUP_PD(pd);
    /* Use void* to stay compilable against older SDK headers */
    typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice, void *, void *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX");
    if (fn) fn(_real, pFeatures, pLimits);
}

#ifdef VK_NV_cooperative_matrix
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceCooperativeMatrixPropertiesNV(
    VkPhysicalDevice                 pd,
    uint32_t                        *pCount,
    VkCooperativeMatrixPropertiesNV *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV fn =
        (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV)
        ext_fn(_si, "vkGetPhysicalDeviceCooperativeMatrixPropertiesNV");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pProps);
}
#endif /* VK_NV_cooperative_matrix */

#ifdef VK_NV_coverage_reduction_mode
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(
    VkPhysicalDevice                       pd,
    uint32_t                              *pCount,
    VkFramebufferMixedSamplesCombinationNV *pCombinations)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *,
        VkFramebufferMixedSamplesCombinationNV *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pCombinations);
}
#endif /* VK_NV_coverage_reduction_mode */

/* ── VK_KHR_display / VK_EXT_display_surface_counter stubs ──────────────────
 * These functions take VkPhysicalDevice as their first argument.  We must wrap
 * them so the loader never passes our StereoPhysicalDevice* wrapper directly
 * to NVIDIA.  The secondary args (VkDisplayKHR, VkDisplayModeKHR, etc.) are
 * not wrapped by VKS3D and pass through unchanged.               */

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceDisplayPropertiesKHR(
    VkPhysicalDevice pd, uint32_t *pCount, VkDisplayPropertiesKHR *pProps)
{
    STEREO_LOG("stereo_GetPhysicalDeviceDisplayPropertiesKHR: pd=%p", (void*)pd);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *, VkDisplayPropertiesKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceDisplayPlanePropertiesKHR(
    VkPhysicalDevice pd, uint32_t *pCount, VkDisplayPlanePropertiesKHR *pProps)
{
    STEREO_LOG("stereo_GetPhysicalDeviceDisplayPlanePropertiesKHR: pd=%p", (void*)pd);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *, VkDisplayPlanePropertiesKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetDisplayPlaneSupportedDisplaysKHR(
    VkPhysicalDevice pd, uint32_t planeIndex, uint32_t *pCount, VkDisplayKHR *pDisplays)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t, uint32_t *, VkDisplayKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetDisplayPlaneSupportedDisplaysKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, planeIndex, pCount, pDisplays);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetDisplayModePropertiesKHR(
    VkPhysicalDevice pd, VkDisplayKHR display,
    uint32_t *pCount, VkDisplayModePropertiesKHR *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, VkDisplayKHR, uint32_t *, VkDisplayModePropertiesKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetDisplayModePropertiesKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, display, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateDisplayModeKHR(
    VkPhysicalDevice pd, VkDisplayKHR display,
    const VkDisplayModeCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDisplayModeKHR *pMode)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, VkDisplayKHR,
        const VkDisplayModeCreateInfoKHR *, const VkAllocationCallbacks *, VkDisplayModeKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkCreateDisplayModeKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, display, pCreateInfo, pAllocator, pMode);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetDisplayPlaneCapabilitiesKHR(
    VkPhysicalDevice pd, VkDisplayModeKHR mode,
    uint32_t planeIndex, VkDisplayPlaneCapabilitiesKHR *pCaps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, VkDisplayModeKHR, uint32_t,
        VkDisplayPlaneCapabilitiesKHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetDisplayPlaneCapabilitiesKHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, mode, planeIndex, pCaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceDisplayProperties2KHR(
    VkPhysicalDevice pd, uint32_t *pCount, VkDisplayProperties2KHR *pProps)
{
    STEREO_LOG("stereo_GetPhysicalDeviceDisplayProperties2KHR: pd=%p", (void*)pd);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *, VkDisplayProperties2KHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceDisplayProperties2KHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceDisplayPlaneProperties2KHR(
    VkPhysicalDevice pd, uint32_t *pCount, VkDisplayPlaneProperties2KHR *pProps)
{
    STEREO_LOG("stereo_GetPhysicalDeviceDisplayPlaneProperties2KHR: pd=%p", (void*)pd);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *, VkDisplayPlaneProperties2KHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceDisplayPlaneProperties2KHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetDisplayModeProperties2KHR(
    VkPhysicalDevice pd, VkDisplayKHR display,
    uint32_t *pCount, VkDisplayModeProperties2KHR *pProps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, VkDisplayKHR,
        uint32_t *, VkDisplayModeProperties2KHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetDisplayModeProperties2KHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, display, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetDisplayPlaneCapabilities2KHR(
    VkPhysicalDevice pd,
    const VkDisplayPlaneInfo2KHR *pDisplayPlaneInfo,
    VkDisplayPlaneCapabilities2KHR *pCaps)
{
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
        const VkDisplayPlaneInfo2KHR *, VkDisplayPlaneCapabilities2KHR *);
    PFN fn = (PFN)ext_fn(_si, "vkGetDisplayPlaneCapabilities2KHR");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, pDisplayPlaneInfo, pCaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceCapabilities2EXT(
    VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilities2EXT *pSurfaceCaps)
{
    STEREO_LOG("stereo_GetPhysicalDeviceSurfaceCapabilities2EXT: pd=%p surface=%p",
               (void*)pd, (void*)(uintptr_t)surface);
    LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
    typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilities2EXT *);
    PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSurfaceCapabilities2EXT");
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return fn(_real, surface, pSurfaceCaps);
}

/* ── Generic physdev trampoline for unknown extensions ───────────────────────
 * Called for any vkGetPhysicalDevice* / vkEnumerateDevice* function that
 * has no explicit wrapper stub.  We store the real function pointer paired
 * with a thunk that translates StereoPhysdev* → real_pd before the call.
 *
 * Architecture: return a closure isn't possible in C, so instead we use a
 * small table of (name → real_fn) and a fixed thunk that reads _spd->real_pd
 * from the VkPhysicalDevice arg before forwarding.  This is safe because the
 * real ICD's function is called with the real handle.
 *
 * Simpler approach: just log and return NULL for truly unknown functions.
 * The loader already handles NULL gracefully for optional extensions.       */
PFN_vkVoidFunction stereo_physdev_trampoline_lookup(StereoInstance *si, const char *name)
{
    /* Look up the real function pointer from the real ICD */
    PFN_vkVoidFunction real_fn = ext_fn(si, name);
    if (!real_fn) {
        STEREO_LOG("physdev_trampoline: '%s' not in real ICD — returning NULL", name);
        return NULL;
    }
    /* We cannot safely return the real ICD's raw function pointer: the loader
     * will call it with our StereoPhysdev* wrapper as VkPhysicalDevice, and
     * the real ICD will dereference it as its own struct → crash.
     * Return NULL and log so we know which stubs to add. */
    STEREO_LOG("physdev_trampoline: '%s' needs a wrapper stub — returning NULL", name);
    return NULL;
}
