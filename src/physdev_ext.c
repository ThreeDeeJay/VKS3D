
#include <string.h>
#include <stdlib.h>
#include "stereo_icd.h"
static inline PFN_vkVoidFunction ext_fn(StereoInstance *si, const char *name)
{
PFN_vkGetInstanceProcAddr pdPA = stereo_get_real_pdPA();
if (pdPA) {
PFN_vkVoidFunction fn = (PFN_vkVoidFunction)pdPA(si->real_instance, name);
if (fn) return fn;
}
return (PFN_vkVoidFunction)si->real_get_instance_proc_addr(si->real_instance, name);
}
#define LOOKUP_PD(pd) \
STEREO_LOG("%s: pd=%p", __func__, (void*)(uintptr_t)(pd)); \
StereoPhysdev *_spd = (StereoPhysdev *)(uintptr_t)(pd); \
StereoInstance *_si = _spd ? _spd->si : NULL; \
if (!_si) { STEREO_ERR("%s: si==NULL for pd=%p (not our wrapper?)", __func__, (void*)(uintptr_t)(pd)); return; } \
VkPhysicalDevice _real = _spd->real_pd
#define LOOKUP_PD_R(pd, err) \
STEREO_LOG("%s: pd=%p", __func__, (void*)(uintptr_t)(pd)); \
StereoPhysdev *_spd = (StereoPhysdev *)(uintptr_t)(pd); \
StereoInstance *_si = _spd ? _spd->si : NULL; \
if (!_si) { STEREO_ERR("%s: si==NULL for pd=%p (not our wrapper?)", __func__, (void*)(uintptr_t)(pd)); return (err); } \
VkPhysicalDevice _real = _spd->real_pd
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{  stereo_GetPhysicalDeviceFeatures2(pd, f); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{  stereo_GetPhysicalDeviceProperties2(pd, p); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p)
{  stereo_GetPhysicalDeviceFormatProperties2(pd, fmt, p); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
{  stereo_GetPhysicalDeviceMemoryProperties2(pd, p); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceQueueFamilyProperties2KHR(
VkPhysicalDevice pd, uint32_t *pCount, VkQueueFamilyProperties2 *pProps)
{  stereo_GetPhysicalDeviceQueueFamilyProperties2(pd, pCount, pProps); }
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceImageFormatProperties2(
VkPhysicalDevice pd,
const VkPhysicalDeviceImageFormatInfo2 *pInfo,
VkImageFormatProperties2 *pProps)
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
{  return stereo_GetPhysicalDeviceImageFormatProperties2(pd, pInfo, pProps); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceSparseImageFormatProperties2(
VkPhysicalDevice pd,
const VkPhysicalDeviceSparseImageFormatInfo2 *pInfo,
uint32_t *pCount,
VkSparseImageFormatProperties2 *pProps)
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
{  stereo_GetPhysicalDeviceSparseImageFormatProperties2(pd, pInfo, pCount, pProps); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalBufferProperties(
VkPhysicalDevice pd,
const VkPhysicalDeviceExternalBufferInfo *pInfo,
VkExternalBufferProperties *pProps)
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
{  stereo_GetPhysicalDeviceExternalBufferProperties(pd, pInfo, pProps); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalFenceProperties(
VkPhysicalDevice pd,
const VkPhysicalDeviceExternalFenceInfo *pInfo,
VkExternalFenceProperties *pProps)
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
{  stereo_GetPhysicalDeviceExternalFenceProperties(pd, pInfo, pProps); }
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceExternalSemaphoreProperties(
VkPhysicalDevice pd,
const VkPhysicalDeviceExternalSemaphoreInfo *pInfo,
VkExternalSemaphoreProperties *pProps)
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
{  stereo_GetPhysicalDeviceExternalSemaphoreProperties(pd, pInfo, pProps); }
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumeratePhysicalDeviceGroups(
VkInstance instance,
uint32_t *pCount,
VkPhysicalDeviceGroupProperties *pProps)
{
StereoInstance *si = stereo_instance_from_handle(instance);
if (!si) return VK_ERROR_INITIALIZATION_FAILED;
typedef VkResult (VKAPI_PTR *PFN)(VkInstance, uint32_t *,
VkPhysicalDeviceGroupProperties *);
PFN fn = (PFN)ext_fn(si, "vkEnumeratePhysicalDeviceGroups");
if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
VkResult res = fn(si->real_instance, pCount, pProps);
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
{  return stereo_EnumeratePhysicalDeviceGroups(instance, pCount, pProps); }
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSurfaceCapabilities2KHR(
VkPhysicalDevice pd,
const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
VkSurfaceCapabilities2KHR *pCaps)
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
VkPhysicalDevice pd,
const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
uint32_t *pCount,
VkSurfaceFormat2KHR *pFormats)
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
VkPhysicalDevice pd,
const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
uint32_t *pCount,
VkPresentModeKHR *pModes)
{
LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
const VkPhysicalDeviceSurfaceInfo2KHR *, uint32_t *, VkPresentModeKHR *);
PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSurfacePresentModes2EXT");
if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
return fn(_real, pInfo, pCount, pModes);
}
#endif 
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDevicePresentRectanglesKHR(
VkPhysicalDevice pd,
VkSurfaceKHR surface,
uint32_t *pRectCount,
VkRect2D *pRects)
{
LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice,
VkSurfaceKHR, uint32_t *, VkRect2D *);
PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDevicePresentRectanglesKHR");
if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
return fn(_real, surface, pRectCount, pRects);
}
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
#endif 
VKAPI_ATTR void VKAPI_CALL
stereo_GetPhysicalDeviceMultisamplePropertiesEXT(
VkPhysicalDevice pd,
VkSampleCountFlagBits samples,
VkMultisamplePropertiesEXT *pProps)
{
LOOKUP_PD(pd);
typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice,
VkSampleCountFlagBits, VkMultisamplePropertiesEXT *);
PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceMultisamplePropertiesEXT");
if (fn) fn(_real, samples, pProps);
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceExternalImageFormatPropertiesNV(
VkPhysicalDevice pd,
VkFormat format,
VkImageType type,
VkImageTiling tiling,
VkImageUsageFlags usage,
VkImageCreateFlags flags,
VkExternalMemoryHandleTypeFlagsNV externalHandleType,
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
void *pFeatures, 
void *pLimits) 
{
LOOKUP_PD(pd);
typedef void (VKAPI_PTR *PFN)(VkPhysicalDevice, void *, void *);
PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX");
if (fn) fn(_real, pFeatures, pLimits);
}
#ifdef VK_NV_cooperative_matrix
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceCooperativeMatrixPropertiesNV(
VkPhysicalDevice pd,
uint32_t *pCount,
VkCooperativeMatrixPropertiesNV *pProps)
{
LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV fn =
(PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV)
ext_fn(_si, "vkGetPhysicalDeviceCooperativeMatrixPropertiesNV");
if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
return fn(_real, pCount, pProps);
}
#endif 
#ifdef VK_NV_coverage_reduction_mode
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(
VkPhysicalDevice pd,
uint32_t *pCount,
VkFramebufferMixedSamplesCombinationNV *pCombinations)
{
LOOKUP_PD_R(pd, VK_ERROR_INITIALIZATION_FAILED);
typedef VkResult (VKAPI_PTR *PFN)(VkPhysicalDevice, uint32_t *,
VkFramebufferMixedSamplesCombinationNV *);
PFN fn = (PFN)ext_fn(_si, "vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV");
if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
return fn(_real, pCount, pCombinations);
}
#endif 
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
PFN_vkVoidFunction stereo_physdev_trampoline_lookup(StereoInstance *si, const char *name)
{
PFN_vkVoidFunction real_fn = ext_fn(si, name);
if (!real_fn) {
STEREO_LOG("physdev_trampoline: '%s' not in real ICD — returning NULL", name);
return NULL;
}
STEREO_LOG("physdev_trampoline: '%s' needs a wrapper stub — returning NULL", name);
return NULL;
}
