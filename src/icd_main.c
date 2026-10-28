
#include <stdio.h>
#include <string.h>
#include "stereo_icd.h"
VKAPI_ATTR void VKAPI_CALL stereo_CmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit *, VkFilter);
VKAPI_ATTR void VKAPI_CALL stereo_CmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy *);
VKAPI_ATTR void VKAPI_CALL stereo_CmdResolveImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageResolve *);
VKAPI_ATTR void VKAPI_CALL stereo_DestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
PFN_vkGetInstanceProcAddr stereo_get_real_pdPA(void);
stereo_dl_t stereo_get_real_icd_handle(void);
static PFN_vkVoidFunction get_instance_proc_addr_internal(
VkInstance instance, const char *name)
{
if (!strcmp(name, "vkCreateInstance"))
return (PFN_vkVoidFunction)stereo_CreateInstance;
if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
return (PFN_vkVoidFunction)stereo_EnumerateInstanceExtensionProperties;
if (!strcmp(name, "vkEnumerateInstanceVersion"))
return (PFN_vkVoidFunction)stereo_EnumerateInstanceVersion;
if (!instance)
return NULL;
StereoInstance *si = stereo_instance_from_handle(instance);
if (!strcmp(name, "vkDestroyInstance"))
return (PFN_vkVoidFunction)stereo_DestroyInstance;
if (!strcmp(name, "vkEnumeratePhysicalDevices"))
return (PFN_vkVoidFunction)stereo_EnumeratePhysicalDevices;
#define PD_FN(fn) if (!strcmp(name, "vk"#fn)) return (PFN_vkVoidFunction)stereo_##fn;
PD_FN(GetPhysicalDeviceProperties)
PD_FN(GetPhysicalDeviceFeatures)
PD_FN(GetPhysicalDeviceMemoryProperties)
PD_FN(GetPhysicalDeviceQueueFamilyProperties)
PD_FN(GetPhysicalDeviceFormatProperties)
PD_FN(GetPhysicalDeviceImageFormatProperties)
PD_FN(GetPhysicalDeviceSparseImageFormatProperties)
PD_FN(EnumerateDeviceExtensionProperties)
PD_FN(EnumerateDeviceLayerProperties)
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
PD_FN(EnumeratePhysicalDeviceGroups)
PD_FN(GetPhysicalDeviceSurfaceSupportKHR)
PD_FN(GetPhysicalDeviceSurfaceCapabilitiesKHR)
PD_FN(GetPhysicalDeviceSurfaceFormatsKHR)
PD_FN(GetPhysicalDeviceSurfacePresentModesKHR)
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
#ifdef VK_KHR_win32_surface
PD_FN(GetPhysicalDeviceWin32PresentationSupportKHR)
#endif
#ifdef VK_EXT_full_screen_exclusive
PD_FN(GetPhysicalDeviceSurfacePresentModes2EXT)
#endif
#ifdef VK_EXT_calibrated_timestamps
PD_FN(GetPhysicalDeviceCalibrateableTimeDomainsEXT)
#endif
PD_FN(GetPhysicalDeviceMultisamplePropertiesEXT)
PD_FN(GetPhysicalDeviceExternalImageFormatPropertiesNV)
PD_FN(GetPhysicalDeviceGeneratedCommandsPropertiesNVX)
#ifdef VK_NV_cooperative_matrix
PD_FN(GetPhysicalDeviceCooperativeMatrixPropertiesNV)
#endif
#ifdef VK_NV_coverage_reduction_mode
PD_FN(GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)
#endif
#undef PD_FN
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
if (!strcmp(name, "vkCreateDevice"))
return (PFN_vkVoidFunction)stereo_CreateDevice;
if (!strcmp(name, "vkGetDeviceProcAddr"))
return (PFN_vkVoidFunction)stereo_GetDeviceProcAddr;
if (!strcmp(name, "vkDestroyDevice"))
return (PFN_vkVoidFunction)stereo_DestroyDevice;
if (!strcmp(name, "vkCreateImageView"))
return (PFN_vkVoidFunction)stereo_CreateImageView;
if (!strcmp(name, "vkCreateFramebuffer"))
return (PFN_vkVoidFunction)stereo_CreateFramebuffer;
if (!strcmp(name, "vkDestroyFramebuffer"))
return (PFN_vkVoidFunction)stereo_DestroyFramebuffer;
if (!strcmp(name, "vkCmdBeginRenderPass"))
return (PFN_vkVoidFunction)stereo_CmdBeginRenderPass;
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
if (!strcmp(name, "vkCreateShadersEXT"))
return (PFN_vkVoidFunction)stereo_CreateShadersEXT;
if (!strcmp(name, "vkDestroyShaderEXT"))
return (PFN_vkVoidFunction)stereo_DestroyShaderEXT;
if (!strcmp(name, "vkCmdBindShadersEXT"))
return (PFN_vkVoidFunction)stereo_CmdBindShadersEXT;
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
if (si && name) {
if (strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
strncmp(name, "vkEnumerateDevice", 17) == 0 ||
strncmp(name, "vkEnumeratePhysicalDeviceGroup", 30) == 0) {
extern PFN_vkVoidFunction stereo_physdev_trampoline_lookup(
StereoInstance *si, const char *name);
PFN_vkVoidFunction t = stereo_physdev_trampoline_lookup(si, name);
if (t) return t;
}
PFN_vkVoidFunction fn =
si->real_get_instance_proc_addr(si->real_instance, name);
if (fn) {
return fn;
}
PFN_vkGetInstanceProcAddr pdPA = stereo_get_real_pdPA();
if (pdPA) {
PFN_vkVoidFunction fp =
pdPA(si->real_instance, name);
return fp;
}
}
return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
stereo_GetDeviceProcAddr(VkDevice device, const char *pName)
{
if (!pName)
return NULL;
#define GDPA_WRAP(name, fn) \
if (!strcmp(pName, name)) { \
return (PFN_vkVoidFunction)fn; \
}
GDPA_WRAP("vkGetDeviceProcAddr",
stereo_GetDeviceProcAddr);
GDPA_WRAP("vkDestroyDevice",
stereo_DestroyDevice);
GDPA_WRAP("vkCreateImage",
stereo_CreateImage);
GDPA_WRAP("vkCreateImageView",
stereo_CreateImageView);
GDPA_WRAP("vkCreateFramebuffer",
stereo_CreateFramebuffer);
GDPA_WRAP("vkDestroyFramebuffer",
stereo_DestroyFramebuffer);
GDPA_WRAP("vkCmdBeginRenderPass",
stereo_CmdBeginRenderPass);
GDPA_WRAP("vkCmdEndRenderPass",
stereo_CmdEndRenderPass);
GDPA_WRAP("vkCmdBeginRendering",
stereo_CmdBeginRendering);
GDPA_WRAP("vkCmdBeginRenderingKHR",
stereo_CmdBeginRendering);
GDPA_WRAP("vkCmdEndRendering",
stereo_CmdEndRendering);
GDPA_WRAP("vkCmdEndRenderingKHR",
stereo_CmdEndRendering);
GDPA_WRAP("vkCmdBindPipeline",
stereo_CmdBindPipeline);
GDPA_WRAP("vkCmdDraw",
stereo_CmdDraw);
GDPA_WRAP("vkCmdDrawIndexed",
stereo_CmdDrawIndexed);
GDPA_WRAP("vkCmdDrawIndirect",
stereo_CmdDrawIndirect);
GDPA_WRAP("vkCmdDrawIndexedIndirect",
stereo_CmdDrawIndexedIndirect);
GDPA_WRAP("vkUpdateDescriptorSets",
stereo_UpdateDescriptorSets);
GDPA_WRAP("vkCmdBindDescriptorSets",
stereo_CmdBindDescriptorSets);
GDPA_WRAP("vkCreateRenderPass",
stereo_CreateRenderPass);
#ifdef VK_KHR_create_renderpass2
GDPA_WRAP("vkCreateRenderPass2KHR",
stereo_CreateRenderPass2KHR);
GDPA_WRAP("vkCreateRenderPass2",
stereo_CreateRenderPass2KHR);
#endif
GDPA_WRAP("vkCreateRayTracingPipelinesKHR",
stereo_CreateRayTracingPipelinesKHR);
GDPA_WRAP("vkCmdTraceRaysKHR",
stereo_CmdTraceRaysKHR);
GDPA_WRAP("vkCmdBlitImage",
stereo_CmdBlitImage);
GDPA_WRAP("vkCmdCopyImage",
stereo_CmdCopyImage);
GDPA_WRAP("vkCmdResolveImage",
stereo_CmdResolveImage);
GDPA_WRAP("vkCreateShaderModule",
stereo_CreateShaderModule);
GDPA_WRAP("vkDestroyShaderModule",
stereo_DestroyShaderModule);
GDPA_WRAP("vkCreateShadersEXT",
stereo_CreateShadersEXT);
GDPA_WRAP("vkDestroyShaderEXT",
stereo_DestroyShaderEXT);
GDPA_WRAP("vkCmdBindShadersEXT",
stereo_CmdBindShadersEXT);
GDPA_WRAP("vkDestroyPipeline",
stereo_DestroyPipeline);
if (!strcmp(pName, "vkCreateGraphicsPipelines")) {
return (PFN_vkVoidFunction)stereo_CreateGraphicsPipelines;
}
GDPA_WRAP("vkCreateSwapchainKHR",
stereo_CreateSwapchainKHR);
GDPA_WRAP("vkDestroySwapchainKHR",
stereo_DestroySwapchainKHR);
GDPA_WRAP("vkGetSwapchainImagesKHR",
stereo_GetSwapchainImagesKHR);
GDPA_WRAP("vkAcquireNextImageKHR",
stereo_AcquireNextImageKHR);
GDPA_WRAP("vkQueuePresentKHR",
stereo_QueuePresentKHR);
#undef GDPA_WRAP
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
for (uint32_t i = 0; i < g_device_count; i++) {
if (g_devices[i].real_device == device ||
(VkDevice)(uintptr_t)&g_devices[i] == device) {
PFN_vkGetDeviceProcAddr real_gdpa =
g_devices[i].real.GetDeviceProcAddr;
if (real_gdpa) {
PFN_vkVoidFunction fp =
real_gdpa(g_devices[i].real_device, pName);
return fp;
}
break;
}
}
return get_instance_proc_addr_internal(
VK_NULL_HANDLE,
pName);
}
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdEnumerateAdapterPhysicalDevices(
VkInstance instance,
LUID adapterLUID,
uint32_t *pPhysicalDeviceCount,
VkPhysicalDevice *pPhysicalDevices)
{
StereoInstance *si = stereo_instance_from_handle(instance);
if (!si) {
STEREO_ERR("vk_icdEnumerateAdapterPhysicalDevices: unknown instance %p", (void*)instance);
return VK_ERROR_INITIALIZATION_FAILED;
}
typedef VkResult (VKAPI_PTR *PFN_EnumAdapter)(
VkInstance, LUID, uint32_t *, VkPhysicalDevice *);
stereo_dl_t real_mod = stereo_get_real_icd_handle();
PFN_EnumAdapter real_fn = real_mod
? (PFN_EnumAdapter)GetProcAddress((HMODULE)real_mod,
"vk_icdEnumerateAdapterPhysicalDevices")
: NULL;
if (!real_fn) {
return si->real.EnumeratePhysicalDevices
? (VkResult)si->real.EnumeratePhysicalDevices(
si->real_instance, pPhysicalDeviceCount, pPhysicalDevices)
: VK_ERROR_INITIALIZATION_FAILED;
}
if (!pPhysicalDevices) {
VkResult r = real_fn(si->real_instance, adapterLUID, pPhysicalDeviceCount, NULL);
return r;
}
uint32_t count = *pPhysicalDeviceCount;
VkPhysicalDevice raw[MAX_PHYSICAL_DEVICES];
if (count > MAX_PHYSICAL_DEVICES) count = MAX_PHYSICAL_DEVICES;
VkResult r = real_fn(si->real_instance, adapterLUID, &count, raw);
if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
for (uint32_t i = 0; i < count; i++) {
StereoPhysdev *spd = stereo_physdev_get_or_create(raw[i], si);
pPhysicalDevices[i] = spd ? (VkPhysicalDevice)(uintptr_t)spd : raw[i];
}
*pPhysicalDeviceCount = count;
return r;
}
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion)
{
if (!pVersion) {
STEREO_ERR("vk_icdNegotiateLoaderICDInterfaceVersion: pVersion is NULL");
return VK_ERROR_INITIALIZATION_FAILED;
}
uint32_t requested = *pVersion;
if (requested > STEREO_ICD_INTERFACE_VERSION)
*pVersion = STEREO_ICD_INTERFACE_VERSION;
return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
if (!pName)
return NULL;
PFN_vkVoidFunction fn = get_instance_proc_addr_internal(instance, pName);
return fn;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
if (!pName)
return NULL;
PFN_vkVoidFunction fn = get_instance_proc_addr_internal(instance, pName);
return fn;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
return stereo_GetDeviceProcAddr(device, pName);
}
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
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *c,
const VkAllocationCallbacks *a, VkImageView *v)
{ return stereo_CreateImageView(d, c, a, v); }
