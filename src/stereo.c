
#include "platform.h"
HANDLE g_vks3d_log_handle = INVALID_HANDLE_VALUE;
int g_vks3d_log_enabled = 0;
#ifndef VKS3D_VERSION
# define VKS3D_VERSION "1.0.0"
#endif
#ifndef VKS3D_GIT_COMMIT
# define VKS3D_GIT_COMMIT "unknown"
#endif
#ifndef VKS3D_BUILD_DATE
# define VKS3D_BUILD_DATE "unknown"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "ini.h"
static StereoInstance g_instances[MAX_INSTANCES];
uint32_t g_instance_count = 0;
static StereoPhysdev g_physdev_wrappers[MAX_PHYSICAL_DEVICES];
static uint32_t g_physdev_count = 0;
StereoDevice g_devices[MAX_DEVICES];
uint32_t g_device_count = 0;
static stereo_mutex_t g_registry_lock;
#ifdef _WIN32
static INIT_ONCE g_registry_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK registry_init_once(PINIT_ONCE io, PVOID p, PVOID *ctx)
{
(void)io; (void)p; (void)ctx;
stereo_mutex_init(&g_registry_lock);
return TRUE;
}
static void ensure_registry_init(void) {
InitOnceExecuteOnce(&g_registry_once, registry_init_once, NULL, NULL);
}
#else
#include <pthread.h>
static pthread_once_t g_registry_once = PTHREAD_ONCE_INIT;
static void registry_init_fn(void) { stereo_mutex_init(&g_registry_lock); }
static void ensure_registry_init(void) { pthread_once(&g_registry_once, registry_init_fn); }
#endif
static stereo_dl_t g_real_icd_handle = STEREO_DL_NULL;
static PFN_vkGetInstanceProcAddr g_real_giPA = NULL;
static PFN_vkGetInstanceProcAddr g_real_pdPA = NULL;
char g_dll_dir[512]; 
char g_exe_dir[512]; 
char g_global_ini[512]; 
char g_local_ini[512]; 
static StereoPresentMode parse_present_mode(const char *s)
{
if (!s || !*s) return STEREO_PRESENT_AUTO;
if (!_stricmp(s, "auto")) return STEREO_PRESENT_AUTO;
if (!_stricmp(s, "dxgi")) return STEREO_PRESENT_DXGI;
if (!_stricmp(s, "dx9")) return STEREO_PRESENT_DX9;
if (!_stricmp(s, "sbs")) return STEREO_PRESENT_SBS;
if (!_stricmp(s, "tab")) return STEREO_PRESENT_TAB;
if (!_stricmp(s, "interlaced")) return STEREO_PRESENT_INTERLACED;
if (!_stricmp(s, "mono")) return STEREO_PRESENT_MONO;
if (!_stricmp(s, "nv3dlib")) return STEREO_PRESENT_NV3DLIB;
return STEREO_PRESENT_AUTO;
}
static float cfg_float(const char *key, float def)
{
float v = def;
char buf[64];
if (ini_read_str(g_global_ini, "VKS3D", key, buf, sizeof(buf))) {
v = (float)atof(buf);
}
if (ini_read_str(g_local_ini, "VKS3D", key, buf, sizeof(buf))) {
v = (float)atof(buf);
}
char env_name[64];
snprintf(env_name, sizeof(env_name), "STEREO_%s", key);
const char *e = stereo_getenv(env_name);
if (e) { v = (float)atof; }
return v;
}
static int cfg_int(const char *key, int def)
{
int v = def;
char buf[32];
if (ini_read_str(g_global_ini, "VKS3D", key, buf, sizeof(buf))) {
v = atoi(buf);
}
if (ini_read_str(g_local_ini, "VKS3D", key, buf, sizeof(buf))) {
v = atoi(buf);
}
return v;
}
static bool cfg_bool(const char *key, bool def)
{
bool v = def;
char buf[16];
if (ini_read_str(g_global_ini, "VKS3D", key, buf, sizeof(buf))) {
v = ini_read_bool(g_global_ini, "VKS3D", key, def);
}
if (ini_read_str(g_local_ini, "VKS3D", key, buf, sizeof(buf))) {
v = ini_read_bool(g_local_ini, "VKS3D", key, v);
}
return v;
}
void stereo_config_init(StereoConfig *cfg)
{
cfg->enabled = cfg_bool("enabled", true);
const char *env_en = stereo_getenv("STEREO_ENABLED");
if (env_en && atoi(env_en) == 0) cfg->enabled = false;
cfg->separation = cfg_float("separation", 0.065f);
cfg->convergence = cfg_float("convergence", 0.030f);
cfg->flip_eyes = cfg_bool ("flip_eyes", false);
char mode_str[32] = "auto";
if (ini_read_str(g_global_ini, "VKS3D", "presentation_mode", mode_str, sizeof(mode_str)))
if (ini_read_str(g_local_ini, "VKS3D", "presentation_mode", mode_str, sizeof(mode_str)))
cfg->present_mode = parse_present_mode(mode_str);
cfg->override_width = (uint32_t)cfg_int("width", 0);
cfg->override_height = (uint32_t)cfg_int("height", 0);
cfg->refresh_rate = (uint32_t)cfg_int("refresh_rate", 120);
cfg->half_fps = cfg_bool("half_fps", false);
cfg->multiview = cfg_bool("multiview", false);
{
const char *shader_objects_mono =
stereo_getenv("VKS3D_SHADER_OBJECTS_MONO");
cfg->shader_objects_mono =
shader_objects_mono && atoi(shader_objects_mono) != 0;
}
cfg->mono_ui = cfg_bool("mono_ui", true);
cfg->sky_flatten = cfg_bool("sky_flatten", true);
cfg->sky_extend = cfg_bool("sky_extend", true);
cfg->projection = cfg_bool("projection", 1);
cfg->step_separation = cfg_float("step_separation", 0.005f);
cfg->step_convergence = cfg_float("step_convergence", 0.005f);
stereo_config_compute_offsets(cfg);
}
void stereo_config_compute_offsets(StereoConfig *cfg)
{
float half_sep = cfg->separation * 0.5f;
if (cfg->projection == STEREO_PROJECTION_PARALLEL)
{
float half_conv = cfg->convergence * 0.5f;
cfg->left_eye_offset = -half_sep + half_conv;
cfg->right_eye_offset = half_sep - half_conv;
}
else 
{
cfg->left_eye_offset = -half_sep;
cfg->right_eye_offset = half_sep;
}
if (cfg->flip_eyes)
{
float t = cfg->left_eye_offset;
cfg->left_eye_offset = cfg->right_eye_offset;
cfg->right_eye_offset = t;
}
}
static bool try_load_icd(const char *path)
{
if (!path || !path[0]) {
return false;
}
stereo_dl_t h = stereo_dl_open(path);
if (!h) {
STEREO_ERR("try_load_icd: LoadLibraryExA failed for '%s': %s"
" (if DriverStore path, check STEREO_REAL_ICD; dependencies"
" should now be found via LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)",
path, stereo_dl_error());
return false;
}
if (stereo_dl_sym(h, "vks3d_internal_marker")) {
stereo_dl_close(h);
return false;
}
PFN_vkGetInstanceProcAddr giPA =
(PFN_vkGetInstanceProcAddr)(uintptr_t)
stereo_dl_sym(h, "vk_icdGetInstanceProcAddr");
if (!giPA) {
giPA = (PFN_vkGetInstanceProcAddr)(uintptr_t)
stereo_dl_sym(h, "vkGetInstanceProcAddr");
}
if (!giPA) {
STEREO_ERR("try_load_icd: '%s' has no usable GetInstanceProcAddr — skipping", path);
stereo_dl_close(h);
return false;
}
g_real_icd_handle = h;
typedef VkResult (VKAPI_PTR *PFN_Negotiate)(uint32_t *);
PFN_Negotiate real_negotiate = (PFN_Negotiate)(uintptr_t)
stereo_dl_sym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
if (real_negotiate) {
uint32_t ver = 5; 
VkResult nr = real_negotiate(&ver);
} else {
}
g_real_giPA = giPA;
g_real_pdPA = (PFN_vkGetInstanceProcAddr)(uintptr_t)
stereo_dl_sym(h, "vk_icdGetPhysicalDeviceProcAddr");
return true;
}
#ifdef _WIN32
static const char *WIN_FALLBACKS_X64[] = {
"nvoglv64.dll",
"amdvlk64.dll",
"C:\\Windows\\System32\\nvoglv64.dll",
"C:\\Windows\\System32\\amdvlk64.dll",
"C:\\Windows\\System32\\igvk64.dll",
NULL
};
static const char *WIN_FALLBACKS_X86[] = {
"nvoglv32.dll",
"amdvlk32.dll",
"C:\\Windows\\SysWOW64\\nvoglv32.dll",
"C:\\Windows\\SysWOW64\\amdvlk32.dll",
"C:\\Windows\\SysWOW64\\igvk32.dll",
NULL
};
bool stereo_load_real_icd(void)
{
if (g_real_icd_handle) return true;
const char *explicit_path = stereo_getenv("STEREO_REAL_ICD");
if (explicit_path) {
if (try_load_icd(explicit_path)) return true;
}
{
char **json_paths = stereo_registry_enum_icd_jsons();
if (json_paths) {
for (int i = 0; json_paths[i]; i++) {
if (strstr(json_paths[i], "VKS3D") || strstr(json_paths[i], "vks3d")) {
free(json_paths[i]); continue;
}
char *dll_path = stereo_json_read_library_path(json_paths[i]);
if (dll_path) {
bool ok = try_load_icd(dll_path);
free(dll_path);
if (ok) {
for (int j = i + 1; json_paths[j]; j++) free(json_paths[j]);
free(json_paths);
return true;
}
} else {
}
free(json_paths[i]);
}
free(json_paths);
} else {
}
}
{
char *ogl_dll = stereo_find_opengl_driver_icd();
if (ogl_dll) {
bool ok = try_load_icd(ogl_dll);
free(ogl_dll);
if (ok) return true;
} else {
}
}
#ifdef _WIN64
const char **fb = WIN_FALLBACKS_X64;
#else
const char **fb = WIN_FALLBACKS_X86;
#endif
for (int i = 0; fb[i]; i++)
if (try_load_icd(fb[i])) return true;
STEREO_ERR("No real GPU ICD found. Set STEREO_REAL_ICD=C:\\path\\to\\icd.dll");
return false;
}
#else 
static const char *LINUX_FALLBACKS[] = {
"/usr/lib/x86_64-linux-gnu/libvulkan_intel.so",
"/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so",
"/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
"/usr/lib64/libvulkan_intel.so",
"/usr/lib64/libvulkan_radeon.so",
"/usr/lib/i386-linux-gnu/libvulkan_intel.so",
"/usr/lib/i386-linux-gnu/libvulkan_radeon.so",
NULL
};
bool stereo_load_real_icd(void)
{
if (g_real_icd_handle) return true;
const char *ep = stereo_getenv("STEREO_REAL_ICD");
if (ep && try_load_icd(ep)) return true;
for (int i = 0; LINUX_FALLBACKS[i]; i++)
if (try_load_icd(LINUX_FALLBACKS[i])) return true;
STEREO_ERR("No real GPU ICD found. Set STEREO_REAL_ICD=/path/to/libvulkan_xxx.so");
return false;
}
#endif
PFN_vkGetInstanceProcAddr stereo_get_real_giPA(void) { return g_real_giPA; }
PFN_vkGetInstanceProcAddr stereo_get_real_pdPA(void) { return g_real_pdPA; }
stereo_dl_t stereo_get_real_icd_handle(void) { return g_real_icd_handle; }
StereoInstance *stereo_instance_alloc(void) {
ensure_registry_init();
stereo_mutex_lock(&g_registry_lock);
if (g_instance_count >= MAX_INSTANCES) {
stereo_mutex_unlock(&g_registry_lock); return NULL;
}
CHECK_ARRAY_COUNT(g_instance_count, MAX_INSTANCES, "g_instance_count");
StereoInstance *si = &g_instances[g_instance_count++];
memset(si, 0, sizeof(*si));
SET_LOADER_MAGIC_VALUE(si); 
stereo_mutex_unlock(&g_registry_lock);
return si;
}
StereoInstance *stereo_instance_from_handle(VkInstance h) {
if (!h) return NULL;
StereoInstance *si = (StereoInstance *)(uintptr_t)h;
if (si >= g_instances && si < g_instances + g_instance_count) {
return si;
}
STEREO_ERR("stereo_instance_from_handle: FAILED to recognise handle %p "
"(our array=[%p,%p), count=%u)",
(void*)h, (void*)g_instances,
(void*)(g_instances + g_instance_count), g_instance_count);
for (uint32_t _j = 0; _j < g_instance_count; _j++) {
STEREO_ERR(" g_instances[%u] = %p (magic=0x%llx real=%p)",
_j, (void*)&g_instances[_j],
(unsigned long long)g_instances[_j].loader_data.loaderMagic,
(void*)g_instances[_j].real_instance);
}
return NULL;
}
void stereo_instance_free(VkInstance h) {
ensure_registry_init();
stereo_mutex_lock(&g_registry_lock);
for (uint32_t i = 0; i < g_instance_count; i++) {
if (g_instances[i].real_instance == h) {
g_instances[i] = g_instances[--g_instance_count]; break;
}
}
stereo_mutex_unlock(&g_registry_lock);
}
StereoPhysdev *stereo_physdev_get_or_create(VkPhysicalDevice real_pd, StereoInstance *si)
{
if (!real_pd || !si) return NULL;
ensure_registry_init();
stereo_mutex_lock(&g_registry_lock);
for (uint32_t i = 0; i < g_physdev_count; i++) {
if (g_physdev_wrappers[i].real_pd == real_pd) {
g_physdev_wrappers[i].si = si; 
StereoPhysdev *spd = &g_physdev_wrappers[i];
stereo_mutex_unlock(&g_registry_lock);
return spd;
}
}
if (g_physdev_count >= MAX_PHYSICAL_DEVICES) {
STEREO_ERR("stereo_physdev_get_or_create: wrapper table full (%u)", g_physdev_count);
stereo_mutex_unlock(&g_registry_lock);
return NULL;
}
CHECK_ARRAY_COUNT(g_physdev_count, MAX_PHYSICAL_DEVICES, "g_physdev_count");
StereoPhysdev *spd = &g_physdev_wrappers[g_physdev_count++];
memset(spd, 0, sizeof(*spd));
SET_LOADER_MAGIC_VALUE(spd); 
spd->real_pd = real_pd;
spd->si = si;
stereo_mutex_unlock(&g_registry_lock);
return spd;
}
StereoInstance *stereo_si_from_physdev(VkPhysicalDevice pd)
{
if (!pd) return NULL;
StereoPhysdev *spd = (StereoPhysdev *)(uintptr_t)pd;
if (spd >= g_physdev_wrappers && spd < g_physdev_wrappers + g_physdev_count) {
return spd->si;
}
STEREO_ERR("stereo_si_from_physdev: unrecognised handle %p (not in wrapper array)", (void*)pd);
return NULL;
}
StereoDevice *stereo_device_alloc(void) {
ensure_registry_init();
stereo_mutex_lock(&g_registry_lock);
if (g_device_count >= MAX_DEVICES) {
stereo_mutex_unlock(&g_registry_lock); return NULL;
}
CHECK_ARRAY_COUNT(g_device_count, MAX_DEVICES, "g_device_count");
StereoDevice *sd = &g_devices[g_device_count++];
memset(sd, 0, sizeof(*sd));
stereo_mutex_init(&sd->lock);
strncpy(sd->global_ini, g_global_ini, sizeof(sd->global_ini) - 1);
strncpy(sd->local_ini, g_local_ini, sizeof(sd->local_ini) - 1);
stereo_mutex_unlock(&g_registry_lock);
return sd;
}
StereoDevice *stereo_device_from_handle(VkDevice h) {
ensure_registry_init();
stereo_mutex_lock(&g_registry_lock);
for (uint32_t i = 0; i < g_device_count; i++) {
if (g_devices[i].real_device == h) {
stereo_mutex_unlock(&g_registry_lock); return &g_devices[i];
}
}
stereo_mutex_unlock(&g_registry_lock);
return NULL;
}
void stereo_device_free(VkDevice h) {
ensure_registry_init();
stereo_mutex_lock(&g_registry_lock);
for (uint32_t i = 0; i < g_device_count; i++) {
if (g_devices[i].real_device == h) {
stereo_mutex_destroy(&g_devices[i].lock);
g_devices[i] = g_devices[--g_device_count]; break;
}
}
stereo_mutex_unlock(&g_registry_lock);
}
StereoSwapchain *stereo_swapchain_lookup(StereoDevice *dev, VkSwapchainKHR sc) {
for (uint32_t i = 0; i < dev->swapchain_count; i++) {
if (dev->swapchains[i].app_handle == sc) return &dev->swapchains[i];
if (dev->swapchains[i].real_swapchain == sc) return &dev->swapchains[i];
}
return NULL;
}
StereoRenderPassInfo *stereo_rp_lookup(StereoDevice *dev, VkRenderPass rp) {
for (uint32_t i = 0; i < dev->render_pass_count; i++)
{
StereoRenderPassInfo *rpi = &dev->render_passes[i];
if (rpi->handle == rp)
{
return rpi;
}
}
return NULL;
}
#define LOAD_INST(table, inst, fn) \
do { \
(table)->fn = (PFN_vk##fn)(g_real_giPA)((inst), "vk"#fn); \
} while (0)
void stereo_populate_instance_dispatch(StereoInstance *si)
{
VkInstance ri = si->real_instance;
LOAD_INST(&si->real, ri, DestroyInstance);
LOAD_INST(&si->real, ri, EnumeratePhysicalDevices);
LOAD_INST(&si->real, ri, GetPhysicalDeviceProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceProperties2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceFeatures);
LOAD_INST(&si->real, ri, GetPhysicalDeviceFeatures2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceMemoryProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceMemoryProperties2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceQueueFamilyProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceQueueFamilyProperties2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceFormatProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceFormatProperties2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceImageFormatProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceSparseImageFormatProperties);
LOAD_INST(&si->real, ri, EnumerateDeviceExtensionProperties);
LOAD_INST(&si->real, ri, EnumerateDeviceLayerProperties);
LOAD_INST(&si->real, ri, CreateDevice);
#define LOAD_SURF(table, ri, fn) \
do { \
PFN_vk##fn _p = NULL; \
if (g_real_pdPA) \
_p = (PFN_vk##fn)(g_real_pdPA)((ri), "vk"#fn); \
if (!_p) \
_p = (PFN_vk##fn)(g_real_giPA)((ri), "vk"#fn); \
(table)->fn = _p; \
} while (0)
LOAD_SURF(&si->real, ri, DestroySurfaceKHR);
LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfaceSupportKHR);
LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfaceCapabilitiesKHR);
LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfaceFormatsKHR);
LOAD_SURF(&si->real, ri, GetPhysicalDeviceSurfacePresentModesKHR);
#undef LOAD_SURF
LOAD_INST(&si->real, ri, GetPhysicalDeviceImageFormatProperties2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceSparseImageFormatProperties2);
LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalBufferProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalFenceProperties);
LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalSemaphoreProperties);
LOAD_INST(&si->real, ri, EnumeratePhysicalDeviceGroups);
LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceCapabilities2KHR);
LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfaceFormats2KHR);
LOAD_INST(&si->real, ri, GetPhysicalDevicePresentRectanglesKHR);
#ifdef VK_KHR_win32_surface
LOAD_INST(&si->real, ri, GetPhysicalDeviceWin32PresentationSupportKHR);
#endif
#ifdef VK_EXT_full_screen_exclusive
LOAD_INST(&si->real, ri, GetPhysicalDeviceSurfacePresentModes2EXT);
#endif
#ifdef VK_EXT_calibrated_timestamps
LOAD_INST(&si->real, ri, GetPhysicalDeviceCalibrateableTimeDomainsEXT);
#endif
LOAD_INST(&si->real, ri, GetPhysicalDeviceMultisamplePropertiesEXT);
LOAD_INST(&si->real, ri, GetPhysicalDeviceExternalImageFormatPropertiesNV);
#ifdef VK_NV_cooperative_matrix
LOAD_INST(&si->real, ri, GetPhysicalDeviceCooperativeMatrixPropertiesNV);
#endif
#ifdef VK_NV_coverage_reduction_mode
LOAD_INST(&si->real, ri, GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV);
#endif
si->real.GetPhysicalDeviceGeneratedCommandsPropertiesNVX =
g_real_giPA(ri, "vkGetPhysicalDeviceGeneratedCommandsPropertiesNVX");
#ifdef VK_KHR_win32_surface
LOAD_INST(&si->real, ri, CreateWin32SurfaceKHR);
#endif
LOAD_INST(&si->real, ri, CreateDebugReportCallbackEXT);
LOAD_INST(&si->real, ri, DestroyDebugReportCallbackEXT);
LOAD_INST(&si->real, ri, DebugReportMessageEXT);
LOAD_INST(&si->real, ri, CreateDebugUtilsMessengerEXT);
LOAD_INST(&si->real, ri, DestroyDebugUtilsMessengerEXT);
LOAD_INST(&si->real, ri, SubmitDebugUtilsMessageEXT);
}
void stereo_populate_device_dispatch(StereoDevice *sd, VkInstance real_inst)
{
#define L(fn) sd->real.fn = (PFN_vk##fn)(g_real_giPA)((real_inst), "vk"#fn)
L(GetDeviceProcAddr); 
L(DestroyDevice); L(GetDeviceQueue); L(QueueSubmit); L(QueueWaitIdle);
L(DeviceWaitIdle); L(AllocateMemory); L(FreeMemory); L(MapMemory);
L(UnmapMemory); L(FlushMappedMemoryRanges); L(InvalidateMappedMemoryRanges);
L(BindBufferMemory); L(BindImageMemory);
L(GetBufferMemoryRequirements); L(GetImageMemoryRequirements);
L(CreateFence); L(DestroyFence); L(ResetFences); L(GetFenceStatus); L(WaitForFences);
L(CreateSemaphore); L(DestroySemaphore);
L(CreateEvent); L(DestroyEvent); L(GetEventStatus); L(SetEvent); L(ResetEvent);
L(CreateQueryPool); L(DestroyQueryPool); L(GetQueryPoolResults);
L(CreateBuffer); L(DestroyBuffer); L(CreateBufferView); L(DestroyBufferView);
L(CreateImage); L(DestroyImage); L(GetImageSubresourceLayout);
L(CreateImageView); L(DestroyImageView);
L(CreateShaderModule); L(DestroyShaderModule);
L(CreateShadersEXT); L(DestroyShaderEXT); L(CmdBindShadersEXT);
L(CmdSetViewportWithCountEXT);
L(CmdSetScissorWithCountEXT);
L(CmdSetDepthTestEnableEXT);
L(CmdSetDepthWriteEnableEXT);
L(CmdSetDepthCompareOpEXT);
L(CmdSetRasterizerDiscardEnableEXT);
L(CmdSetPolygonModeEXT);
L(CmdSetRasterizationSamplesEXT);
L(CmdSetAlphaToCoverageEnableEXT);
L(CmdSetDepthBiasEnableEXT);
L(CmdSetStencilTestEnableEXT);
L(CmdSetPrimitiveRestartEnableEXT);
L(CmdSetSampleMaskEXT);
L(CmdSetColorBlendEnableEXT);
L(CmdSetColorWriteMaskEXT);
L(CreatePipelineCache); L(DestroyPipelineCache);
L(GetPipelineCacheData); L(MergePipelineCaches);
L(CreateGraphicsPipelines); L(CreateComputePipelines); L(DestroyPipeline);
L(CreateRayTracingPipelinesKHR); L(CmdTraceRaysKHR);
L(CreatePipelineLayout); L(DestroyPipelineLayout);
L(CreateSampler); L(DestroySampler);
L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
L(CreateDescriptorPool); L(DestroyDescriptorPool);
L(ResetDescriptorPool); L(AllocateDescriptorSets); L(FreeDescriptorSets);
L(UpdateDescriptorSets);
L(CreateFramebuffer); L(DestroyFramebuffer);
L(CreateRenderPass); L(DestroyRenderPass); L(GetRenderAreaGranularity);
L(CreateCommandPool); L(DestroyCommandPool); L(ResetCommandPool);
L(AllocateCommandBuffers); L(FreeCommandBuffers);
L(BeginCommandBuffer); L(EndCommandBuffer); L(ResetCommandBuffer);
L(CmdBindPipeline); L(CmdSetViewport); L(CmdSetScissor);
L(CmdSetCullMode); L(CmdSetFrontFace); L(CmdSetPrimitiveTopology);
L(CmdSetCullModeEXT); L(CmdSetFrontFaceEXT); L(CmdSetPrimitiveTopologyEXT);
L(CmdSetVertexInputEXT);
L(CmdSetLineWidth); L(CmdSetDepthBias); L(CmdSetBlendConstants);
L(CmdSetDepthBounds); L(CmdSetStencilCompareMask);
L(CmdSetStencilWriteMask); L(CmdSetStencilReference);
L(CmdBindDescriptorSets); L(CmdBindIndexBuffer); L(CmdBindVertexBuffers);
L(CmdDraw); L(CmdDrawIndexed); L(CmdDrawIndirect); L(CmdDrawIndexedIndirect);
L(CmdDispatch); L(CmdDispatchIndirect);
L(CmdCopyBuffer); L(CmdCopyImage); L(CmdBlitImage);
L(CmdCopyBufferToImage); L(CmdCopyImageToBuffer);
L(CmdUpdateBuffer); L(CmdFillBuffer);
L(CmdClearColorImage); L(CmdClearDepthStencilImage); L(CmdClearAttachments);
L(CmdResolveImage); L(CmdSetEvent); L(CmdResetEvent); L(CmdWaitEvents);
L(CmdPipelineBarrier); L(CmdBeginQuery); L(CmdEndQuery);
L(CmdResetQueryPool); L(CmdWriteTimestamp); L(CmdCopyQueryPoolResults);
L(CmdPushConstants);
L(CmdBeginRenderPass); L(CmdNextSubpass); L(CmdEndRenderPass);
L(CmdBeginRendering);
L(CmdEndRendering);
L(CmdExecuteCommands);
L(CreateSwapchainKHR); L(DestroySwapchainKHR);
L(GetSwapchainImagesKHR); L(AcquireNextImageKHR); L(QueuePresentKHR);
sd->real.CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)
g_real_giPA(real_inst, "vkCreateRenderPass2KHR");
sd->real.GetMemoryWin32HandlePropertiesKHR =
(PFN_vkGetMemoryWin32HandlePropertiesKHR)
g_real_giPA(real_inst, "vkGetMemoryWin32HandlePropertiesKHR"); 
sd->real.ImportMemoryWin32HandleKHR =
(PFN_vkImportMemoryWin32HandleKHR)
g_real_giPA(real_inst,
"vkImportMemoryWin32HandleKHR");
sd->real.ImportSemaphoreWin32HandleKHR =
(PFN_vkImportSemaphoreWin32HandleKHR)
g_real_giPA(real_inst,
"vkImportSemaphoreWin32HandleKHR");
sd->real.CmdDraw = (PFN_vkCmdDraw)
g_real_giPA(real_inst, "vkCmdDraw");
sd->real.CmdDrawIndexed = (PFN_vkCmdDrawIndexed)
g_real_giPA(real_inst, "vkCmdDrawIndexed");
sd->real.CmdDrawIndirect = (PFN_vkCmdDrawIndirect)
g_real_giPA(real_inst, "vkCmdDrawIndirect");
sd->real.CmdDrawIndexedIndirect = (PFN_vkCmdDrawIndexedIndirect)
g_real_giPA(real_inst, "vkCmdDrawIndexedIndirect");
#undef L
}
#ifdef _WIN32
static bool s_nvapi_early_done = false;
void dx9_nvapi_early_init(void)
{
if (s_nvapi_early_done) return;
s_nvapi_early_done = true;
typedef void* (*PFN_NvQI)(unsigned);
typedef int (*PFN_NvFn)(int);
typedef int (*PFN_NvInit)(void);
#ifdef _WIN64
const char *nvapi_dll = "nvapi64.dll";
#else
const char *nvapi_dll = "nvapi.dll";
#endif
HMODULE hNvAPI = LoadLibraryA(nvapi_dll);
if (!hNvAPI) {
return;
}
PFN_NvQI fnQI = (PFN_NvQI)GetProcAddress(hNvAPI, "nvapi_QueryInterface");
if (!fnQI) {
FreeLibrary(hNvAPI);
return;
}
PFN_NvInit fnInit = (PFN_NvInit)fnQI(0x0150E828u);
if (fnInit) {
int r = fnInit();
}
{
static const unsigned sdm_ids[] = {
0x5E8F1974u, 
0xF423435Eu, 
0x3FC40AA5u, 
0
};
bool sdm_ok = false;
for (int k = 0; sdm_ids[k] && !sdm_ok; k++) {
PFN_NvFn fnMode = (PFN_NvFn)fnQI(sdm_ids[k]);
if (fnMode) {
int r = fnMode(2 );
sdm_ok = true;
}
}
if (!sdm_ok)
}
{
PFN_NvFn fnEnable = (PFN_NvFn)fnQI(0x239C4545u );
if (fnEnable) {
int r = fnEnable(0);
}
}
}
#endif 
static void dir_of_path(const char *full, char *out, size_t out_size)
{
size_t len = strlen(full);
const char *p = full + len;
while (p > full && *p != '\\' && *p != '/') p--;
size_t dlen = (size_t)(p - full);
if (dlen == 0) { out[0] = '.'; out[1] = '\0'; return; }
if (dlen >= out_size) dlen = out_size - 1;
memcpy(out, full, dlen);
out[dlen] = '\0';
}
#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
(void)lpvReserved;
if (fdwReason == DLL_PROCESS_ATTACH) {
DisableThreadLibraryCalls(hinstDLL);
vks3d_log_open();
vks3d_log_write("\n\n"); 
{
char dll_path[512] = "<unknown>";
GetModuleFileNameA(hinstDLL, dll_path, sizeof(dll_path));
dir_of_path(dll_path, g_dll_dir, sizeof(g_dll_dir));
snprintf(g_global_ini, sizeof(g_global_ini), "%s\\vks3d.ini", g_dll_dir);
}
{
char exe_path[512] = "<unknown>";
GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
dir_of_path(exe_path, g_exe_dir, sizeof(g_exe_dir));
snprintf(g_local_ini, sizeof(g_local_ini), "%s\\vks3d.ini", g_exe_dir);
char local_vk[512];
snprintf(local_vk, sizeof(local_vk), "%s\\vulkan-1.dll", g_exe_dir);
if (GetFileAttributesA(local_vk) != INVALID_FILE_ATTRIBUTES)
else
}
{
char pm_str[64] = "";
ini_read_str(g_global_ini, "VKS3D", "presentation_mode", pm_str, sizeof(pm_str));
ini_read_str(g_local_ini, "VKS3D", "presentation_mode", pm_str, sizeof(pm_str));
{
char env_pm[64] = "";
DWORD n = GetEnvironmentVariableA("STEREO_PRESENTATION_MODE", env_pm, sizeof(env_pm));
if (n > 0 && n < sizeof(env_pm)) memcpy(pm_str, env_pm, n + 1);
}
if (_stricmp(pm_str, "dx9") == 0) {
dx9_nvapi_early_init();
} else {
}
}
{
const char *key64 = "SOFTWARE\\Khronos\\Vulkan\\Drivers";
const char *key32 = "SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\Drivers";
const char *icd_key =
#ifdef _WIN64
key64;
#else
key32;
#endif
HKEY hk;
if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, icd_key, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
int active = 0, vks3d_found = 0;
char val_name[512];
char json_api_ver[32];
DWORD val_name_sz, val_type, val_data, val_data_sz, idx = 0;
while (1) {
val_name_sz = sizeof(val_name);
val_data_sz = sizeof(val_data);
LONG r = RegEnumValueA(hk, idx++, val_name, &val_name_sz,
NULL, &val_type, (BYTE*)&val_data, &val_data_sz);
if (r != ERROR_SUCCESS) break;
if (val_type != REG_DWORD) continue;
if (val_data != 0) continue; 
active++;
if (strstr(val_name, "VKS3D") || strstr(val_name, "vks3d")) {
FILE *jf;
int major, minor;
vks3d_found = 1;
json_api_ver[0] = '\0';
jf = fopen(val_name, "r");
if (jf) {
char line[1024];
while (fgets(line, sizeof(line), jf)) {
char *p = strstr(line, "api_version");
char *q1, *q2;
size_t n;
if (!p) continue;
q1 = strchr(p, '"'); if (!q1) continue;
q2 = strchr(q1+1, '"'); if (!q2) continue;
q1 = q2 + 1;
q2 = strchr(q1, '"'); if (!q2) continue;
n = (size_t)(q2 - q1);
if (n >= sizeof(json_api_ver)) n = sizeof(json_api_ver) - 1;
memcpy(json_api_ver, q1, n);
json_api_ver[n] = '\0';
break;
}
fclose(jf);
}
if (!json_api_ver[0])
strncpy(json_api_ver, "(not found)", sizeof(json_api_ver)-1);
major = 0; minor = 0;
if (sscanf(json_api_ver, "%d.%d", &major, &minor) == 2) {
if (major > 1 || (major == 1 && minor > 1)) {
STEREO_ERR(
"DllMain: WARNING: JSON api_version='%s' is Vulkan %d.%d — "
"Vulkan 1.1.x loaders (e.g. driver 426.06) will SILENTLY "
"SKIP VKS3D without loading the DLL, producing no log and "
"2D output. Re-run install.bat to rewrite the JSON with "
"api_version=1.1.0.",
json_api_ver, major, minor);
}
}
} else {
}
}
RegCloseKey(hk);
if (active > 1 || (active == 1 && !vks3d_found)) {
} else {
}
}
}
} else if (fdwReason == DLL_PROCESS_DETACH) {
}
return TRUE;
}
STEREO_EXPORT void vks3d_internal_marker(void) {}
#endif
