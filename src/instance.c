
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "stereo_icd.h"
static const char *STEREO_EXTRA_INSTANCE_EXTS[] = {
"VK_KHR_get_physical_device_properties2",
"VK_EXT_debug_utils",
};
#define STEREO_EXTRA_INST_EXT_COUNT \
(sizeof(STEREO_EXTRA_INSTANCE_EXTS) / sizeof(STEREO_EXTRA_INSTANCE_EXTS[0]))
bool stereo_load_real_icd(void);
PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void);
StereoInstance *stereo_instance_alloc(void);
void stereo_populate_instance_dispatch(StereoInstance *si);
StereoPhysdev *stereo_physdev_get_or_create(VkPhysicalDevice real_pd, StereoInstance *si);
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
if (eiv) {
VkResult r = eiv(pApiVersion);
if (r == VK_SUCCESS) {
const uint32_t MAX_VER =
#ifdef VK_MAKE_API_VERSION
VK_MAKE_API_VERSION(0, 1, 3, 0);
#else
VK_MAKE_VERSION(1, 3, 0);
#endif
if (*pApiVersion > MAX_VER) {
*pApiVersion = MAX_VER;
} else {
}
}
return r;
}
*pApiVersion = VK_API_VERSION_1_0;
return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumerateInstanceExtensionProperties(
const char *pLayerName,
uint32_t *pPropertyCount,
VkExtensionProperties *pProperties)
{
if (!stereo_load_real_icd()) {
STEREO_ERR("stereo_EnumerateInstanceExtensionProperties: no real ICD");
return VK_ERROR_INITIALIZATION_FAILED;
}
PFN_vkGetInstanceProcAddr giPA = stereo_get_real_giPA();
PFN_vkEnumerateInstanceExtensionProperties eiep =
(PFN_vkEnumerateInstanceExtensionProperties)
giPA(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
if (!eiep)
return VK_ERROR_INITIALIZATION_FAILED;
uint32_t real_count = 0;
VkResult res = eiep(pLayerName, &real_count, NULL);
if (res != VK_SUCCESS && res != VK_INCOMPLETE)
return res;
VkExtensionProperties *real_props = NULL;
if (real_count > 0) {
real_props = malloc(real_count * sizeof(VkExtensionProperties));
if (!real_props) return VK_ERROR_OUT_OF_HOST_MEMORY;
res = eiep(pLayerName, &real_count, real_props);
}
uint32_t total = real_count;
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
for (uint32_t r = 0; r < real_count && write < *pPropertyCount; r++) {
pProperties[write++] = real_props[r];
}
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
static VkBool32 VKAPI_CALL
vks3d_debug_callback(
VkDebugUtilsMessageSeverityFlagBitsEXT severity,
VkDebugUtilsMessageTypeFlagsEXT type,
const VkDebugUtilsMessengerCallbackDataEXT *data,
void *user)
{
(void)type; (void)user;
const char *sev = (severity & 0x1000) ? "[VK][E]" :
(severity & 0x0100) ? "[VK][W]" :
(severity & 0x0010) ? "[VK][I]" : "[VK][V]";
if (data && data->pMessage)
return VK_FALSE; 
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateInstance(
const VkInstanceCreateInfo *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkInstance *pInstance)
{
if (!stereo_load_real_icd()) {
STEREO_ERR("stereo_CreateInstance: no real ICD");
return VK_ERROR_INITIALIZATION_FAILED;
}
PFN_vkGetInstanceProcAddr giPA = stereo_get_real_giPA();
PFN_vkCreateInstance real_ci =
(PFN_vkCreateInstance)giPA(VK_NULL_HANDLE, "vkCreateInstance");
if (!real_ci)
return VK_ERROR_INITIALIZATION_FAILED;
VkInstanceCreateInfo ci = *pCreateInfo;
uint32_t orig_count = ci.enabledExtensionCount;
const char **new_exts = malloc((orig_count + STEREO_EXTRA_INST_EXT_COUNT)
* sizeof(char*));
if (!new_exts) return VK_ERROR_OUT_OF_HOST_MEMORY;
for (uint32_t i = 0; i < orig_count; i++)
new_exts[i] = ci.ppEnabledExtensionNames[i];
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
ci.enabledExtensionCount = total;
ci.ppEnabledExtensionNames = new_exts;
VkApplicationInfo app_info;
uint32_t app_ver = VK_API_VERSION_1_0;
if (ci.pApplicationInfo) {
app_info = *ci.pApplicationInfo;
app_ver = app_info.apiVersion;
} else {
memset(&app_info, 0, sizeof(app_info));
app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
}
if (app_ver < VK_API_VERSION_1_1)
app_info.apiVersion = VK_API_VERSION_1_1;
ci.pApplicationInfo = &app_info;
for (uint32_t _i = 0; _i < ci.enabledExtensionCount; _i++)
VkInstance real_inst = VK_NULL_HANDLE;
VkResult res = real_ci(&ci, pAllocator, &real_inst);
free(new_exts);
if (res != VK_SUCCESS)
return res;
StereoInstance *si = stereo_instance_alloc();
if (!si) {
PFN_vkDestroyInstance di =
(PFN_vkDestroyInstance)giPA(real_inst, "vkDestroyInstance");
if (di) di(real_inst, pAllocator);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
si->real_instance = real_inst;
si->real_get_instance_proc_addr = giPA;
stereo_config_init(&si->stereo);
stereo_populate_instance_dispatch(si);
if (si->real.CreateDebugUtilsMessengerEXT) {
VkDebugUtilsMessengerCreateInfoEXT dbg = {
.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
.messageSeverity = 0x1111, 
.messageType = 0x7, 
.pfnUserCallback = vks3d_debug_callback,
};
VkResult _dm = si->real.CreateDebugUtilsMessengerEXT(
real_inst, &dbg, NULL, &si->debug_messenger);
}
*pInstance = (VkInstance)(uintptr_t)si;
return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
StereoInstance *si = stereo_instance_from_handle(instance);
if (!si) return;
si->real.DestroyInstance(si->real_instance, pAllocator);
stereo_instance_free(instance);
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_EnumeratePhysicalDevices(
VkInstance instance,
uint32_t *pPhysicalDeviceCount,
VkPhysicalDevice *pPhysicalDevices)
{
StereoInstance *si = stereo_instance_from_handle(instance);
if (!si) {
STEREO_ERR("stereo_EnumeratePhysicalDevices: unknown instance handle %p", (void*)instance);
return VK_ERROR_INITIALIZATION_FAILED;
}
VkResult res = si->real.EnumeratePhysicalDevices(
si->real_instance, pPhysicalDeviceCount, pPhysicalDevices);
if (res == VK_SUCCESS && pPhysicalDevices) {
for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
VkPhysicalDevice real_pd = pPhysicalDevices[i];
StereoPhysdev *spd = stereo_physdev_get_or_create(real_pd, si);
if (!spd) {
STEREO_ERR("stereo_EnumeratePhysicalDevices: wrapper alloc failed for physdev[%u]", i);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
pPhysicalDevices[i] = (VkPhysicalDevice)(uintptr_t)spd;
}
}
return res;
}
#define LOOKUP_SI(inst) \
StereoInstance *si = stereo_instance_from_handle(inst); \
if (!si) return
#define LOOKUP_SI_R(inst, err) \
StereoInstance *si = stereo_instance_from_handle(inst); \
if (!si) return (err)
VKAPI_ATTR void VKAPI_CALL
stereo_DestroySurfaceKHR(
VkInstance instance, VkSurfaceKHR surface,
const VkAllocationCallbacks *pAllocator)
{
LOOKUP_SI(instance);
if (si->real.DestroySurfaceKHR)
si->real.DestroySurfaceKHR(si->real_instance, surface, pAllocator);
}
#ifdef VK_KHR_win32_surface
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateWin32SurfaceKHR(
VkInstance instance,
const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkSurfaceKHR *pSurface)
{
LOOKUP_SI_R(instance, VK_ERROR_INITIALIZATION_FAILED);
if (!si->real.CreateWin32SurfaceKHR)
return VK_ERROR_EXTENSION_NOT_PRESENT;
VkResult _r = si->real.CreateWin32SurfaceKHR(
si->real_instance, pCreateInfo, pAllocator, pSurface);
if (_r) STEREO_ERR("stereo_CreateWin32SurfaceKHR: real returned %d", (int)_r);
else {
if (pSurface && pCreateInfo &&
si->surface_hwnd_count < MAX_SURFACES) {
si->surface_hwnd[si->surface_hwnd_count].surface = *pSurface;
si->surface_hwnd[si->surface_hwnd_count].hwnd = pCreateInfo->hwnd;
si->surface_hwnd_count++;
}
}
return _r;
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateDebugReportCallbackEXT(
VkInstance instance,
const VkDebugReportCallbackCreateInfoEXT *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkDebugReportCallbackEXT *pCallback)
{
LOOKUP_SI_R(instance, VK_ERROR_INITIALIZATION_FAILED);
if (!si->real.CreateDebugReportCallbackEXT)
return VK_ERROR_EXTENSION_NOT_PRESENT;
return si->real.CreateDebugReportCallbackEXT(
si->real_instance, pCreateInfo, pAllocator, pCallback);
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyDebugReportCallbackEXT(
VkInstance instance, VkDebugReportCallbackEXT callback,
const VkAllocationCallbacks *pAllocator)
{
LOOKUP_SI(instance);
if (si->real.DestroyDebugReportCallbackEXT)
si->real.DestroyDebugReportCallbackEXT(si->real_instance, callback, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL
stereo_DebugReportMessageEXT(
VkInstance instance, VkDebugReportFlagsEXT flags,
VkDebugReportObjectTypeEXT objectType, uint64_t object,
size_t location, int32_t messageCode,
const char *pLayerPrefix, const char *pMessage)
{
LOOKUP_SI(instance);
if (si->real.DebugReportMessageEXT)
si->real.DebugReportMessageEXT(si->real_instance, flags, objectType,
object, location, messageCode, pLayerPrefix, pMessage);
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateDebugUtilsMessengerEXT(
VkInstance instance,
const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkDebugUtilsMessengerEXT *pMessenger)
{
LOOKUP_SI_R(instance, VK_ERROR_INITIALIZATION_FAILED);
if (!si->real.CreateDebugUtilsMessengerEXT)
return VK_ERROR_EXTENSION_NOT_PRESENT;
return si->real.CreateDebugUtilsMessengerEXT(
si->real_instance, pCreateInfo, pAllocator, pMessenger);
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyDebugUtilsMessengerEXT(
VkInstance instance, VkDebugUtilsMessengerEXT messenger,
const VkAllocationCallbacks *pAllocator)
{
LOOKUP_SI(instance);
if (si->real.DestroyDebugUtilsMessengerEXT)
si->real.DestroyDebugUtilsMessengerEXT(si->real_instance, messenger, pAllocator);
}
VKAPI_ATTR void VKAPI_CALL
stereo_SubmitDebugUtilsMessageEXT(
VkInstance instance,
VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
VkDebugUtilsMessageTypeFlagsEXT messageTypes,
const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData)
{
LOOKUP_SI(instance);
if (si->real.SubmitDebugUtilsMessageEXT)
si->real.SubmitDebugUtilsMessageEXT(
si->real_instance, messageSeverity, messageTypes, pCallbackData);
}
