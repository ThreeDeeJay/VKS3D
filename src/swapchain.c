
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"
#include "present_alt.h"
#include "present_nv3d.h"
static uint32_t find_memory_type(StereoDevice *sd, uint32_t type_bits,
VkMemoryPropertyFlags props)
{
VkPhysicalDeviceMemoryProperties mp;
sd->si->real.GetPhysicalDeviceMemoryProperties(sd->real_physdev, &mp);
for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
if ((type_bits & (1u << i)) &&
(mp.memoryTypes[i].propertyFlags & props) == props)
return i;
return UINT32_MAX;
}
static void tracked_destroy_image(
StereoDevice *sd,
VkImage image,
const char *site);
static VkResult alloc_external_stereo_image(StereoDevice *sd, StereoSwapchain *sc,
VkImage *out_image, VkDeviceMemory *out_mem)
{
VkExternalMemoryImageCreateInfo ext_img = {
.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
};
VkImageCreateInfo ici = {
.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
.pNext = &ext_img,
.imageType = VK_IMAGE_TYPE_2D,
.format = sc->format,
.extent = {sc->app_width, sc->app_height, 1},
.mipLevels = 1,
.arrayLayers = 2,
.samples = VK_SAMPLE_COUNT_1_BIT,
.tiling = VK_IMAGE_TILING_OPTIMAL,
.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
};
VkResult res = sd->real.CreateImage(sd->real_device, &ici, NULL, out_image);
if (res != VK_SUCCESS) { STEREO_ERR("CreateImage(external) failed: %d", res); return res; }
VkMemoryRequirements mr;
sd->real.GetImageMemoryRequirements(sd->real_device, *out_image, &mr);
if (!sd->real.GetMemoryWin32HandlePropertiesKHR) {
STEREO_ERR("GetMemoryWin32HandlePropertiesKHR not loaded");
tracked_destroy_image(
sd,
*out_image,
"external image failure");
return VK_ERROR_EXTENSION_NOT_PRESENT;
}
VkMemoryWin32HandlePropertiesKHR hp = {
.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
res = sd->real.GetMemoryWin32HandlePropertiesKHR(sd->real_device,
VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, sc->shared_nt_handle, &hp);
if (res != VK_SUCCESS) {
STEREO_ERR("GetMemoryWin32HandlePropertiesKHR failed: %d", res);
tracked_destroy_image(
sd,
*out_image,
"external image failure"); return res;
}
uint32_t mt = find_memory_type(sd, mr.memoryTypeBits & hp.memoryTypeBits,
VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
if (mt == UINT32_MAX) {
STEREO_ERR("No compatible memory for external image");
tracked_destroy_image(
sd,
*out_image,
"external image failure");
return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
VkImportMemoryWin32HandleInfoKHR import_info = {
.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
.handle = sc->shared_nt_handle,
};
VkMemoryAllocateInfo mai = {
.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import_info,
.allocationSize = mr.size, .memoryTypeIndex = mt,
};
res = sd->real.AllocateMemory(sd->real_device, &mai, NULL, out_mem);
if (res != VK_SUCCESS) {
STEREO_ERR("AllocateMemory(import) failed: %d", res);
tracked_destroy_image(
sd,
*out_image,
"external image failure"); return res;
}
sc->shared_nt_handle = NULL;
res = sd->real.BindImageMemory(sd->real_device, *out_image, *out_mem, 0);
if (res != VK_SUCCESS) {
STEREO_ERR("BindImageMemory(external) failed: %d", res);
sd->real.FreeMemory(sd->real_device, *out_mem, NULL);
tracked_destroy_image(
sd,
*out_image,
"external image failure");
}
return res;
}
static bool setup_barrier_resources(StereoDevice *sd, StereoSwapchain *sc)
{
VkCommandPoolCreateInfo cpci = {
.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
.queueFamilyIndex = sd->gfx_qf,
};
if (sd->real.CreateCommandPool(sd->real_device, &cpci, NULL, &sc->barrier_pool)
!= VK_SUCCESS) return false;
VkCommandBufferAllocateInfo cbai = {
.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
.commandPool = sc->barrier_pool,
.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
.commandBufferCount = 1,
};
if (sd->real.AllocateCommandBuffers(sd->real_device, &cbai, sc->barrier_cmds)
!= VK_SUCCESS) return false;
VkFenceCreateInfo fci = {
.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
.flags = VK_FENCE_CREATE_SIGNALED_BIT, 
};
return sd->real.CreateFence(sd->real_device, &fci, NULL, &sc->barrier_fences[0])
== VK_SUCCESS;
}
static VkResult alloc_alt_stereo_swapchain(StereoDevice *sd, StereoSwapchain *sc)
{
sc->image_count = 1;
sc->stereo_images = calloc(1, sizeof(VkImage));
sc->stereo_memory = calloc(1, sizeof(VkDeviceMemory));
sc->stereo_views_arr = calloc(1, sizeof(VkImageView));
sc->barrier_cmds = calloc(1, sizeof(VkCommandBuffer));
sc->barrier_fences = calloc(1, sizeof(VkFence));
if (!sc->stereo_images || !sc->stereo_memory || !sc->stereo_views_arr ||
!sc->barrier_cmds || !sc->barrier_fences)
return VK_ERROR_OUT_OF_HOST_MEMORY;
VkResult res = alt_alloc_stereo_image(sd, sc,
&sc->stereo_images[0], &sc->stereo_memory[0]);
if (res != VK_SUCCESS) return res;
VkImageViewCreateInfo vci = {
.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
.image = sc->stereo_images[0],
.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
.format = sc->format,
.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 },
};
res = sd->real.CreateImageView(sd->real_device, &vci, NULL, &sc->stereo_views_arr[0]);
if (res != VK_SUCCESS) return res;
if (sd->upgraded_view_count < MAX_UPGRADED_VIEWS)
{
CHECK_ARRAY_COUNT(sd->upgraded_view_count, MAX_UPGRADED_VIEWS, "upgraded_view_count");
sd->upgraded_views[sd->upgraded_view_count++] =
sc->stereo_views_arr[0];
}
return VK_SUCCESS;
}
static void remove_tracked_image(
VkImage *arr,
uint32_t *count,
VkImage image)
{
for (uint32_t i = 0; i < *count; i++)
{
if (arr[i] == image)
{
uint32_t last = --(*count);
arr[i] = arr[last];
return;
}
}
}
static void tracked_destroy_image(
StereoDevice *sd,
VkImage image,
const char *site)
{
remove_tracked_image(
sd->intercepted_depth,
&sd->intercepted_depth_count,
image);
remove_tracked_image(
sd->intercepted_color,
&sd->intercepted_color_count,
image);
sd->real.DestroyImage(
sd->real_device,
image,
NULL);
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateSwapchainKHR(VkDevice device,
const VkSwapchainCreateInfoKHR *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkSwapchainKHR *pSwapchain)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
bool has_free_slot = false;
for (uint32_t fi = 0; fi < sd->swapchain_count; fi++)
if (sd->swapchains[fi].app_handle == VK_NULL_HANDLE) { has_free_slot = true; break; }
if (!sd->stereo.enabled || (sd->swapchain_count >= MAX_SWAPCHAINS && !has_free_slot))
return sd->real.CreateSwapchainKHR(sd->real_device, pCreateInfo, pAllocator, pSwapchain);
uint32_t app_w = pCreateInfo->imageExtent.width;
uint32_t app_h = pCreateInfo->imageExtent.height;
bool slot_is_new = false;
StereoSwapchain *old_sc = NULL;
if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
{
old_sc =
stereo_swapchain_lookup(
sd,
pCreateInfo->oldSwapchain);
}
if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
{
old_sc =
stereo_swapchain_lookup(
sd,
pCreateInfo->oldSwapchain);
}
StereoSwapchain *sc;
if (old_sc)
{
sc = old_sc;
sc->resize_reused = true;
}
else
{
uint32_t slot = sd->swapchain_count;
for (uint32_t fi = 0; fi < sd->swapchain_count; fi++)
if (sd->swapchains[fi].app_handle == VK_NULL_HANDLE) { slot = fi; break; }
slot_is_new = (slot == sd->swapchain_count);
sc = &sd->swapchains[slot];
memset(sc, 0, sizeof(*sc));
sc->resize_reused = false;
}
sc->device = sd->real_device;
sc->app_width = app_w;
sc->app_height = app_h;
sc->format = pCreateInfo->imageFormat;
sc->hwnd = stereo_si_hwnd_for_surface(sd->si, pCreateInfo->surface);
StereoPresentMode req = sd->stereo.present_mode;
if (req == STEREO_PRESENT_NV3DLIB)
{
if (!nv3d_init(sd, app_w, app_h))
{
STEREO_ERR("[NV3D] init failed");
return VK_ERROR_INITIALIZATION_FAILED;
}
if (sc->stereo_images)
{
}
VkResult nvres =
alloc_alt_stereo_swapchain(sd, sc);
if (nvres == VK_SUCCESS)
{
if (!setup_barrier_resources(sd, sc))
{
STEREO_ERR(
"[NV3D] setup_barrier_resources failed");
nvres = VK_ERROR_INITIALIZATION_FAILED;
}
}
if (nvres != VK_SUCCESS)
{
STEREO_ERR(
"[NV3D] alloc_alt_stereo_swapchain failed");
goto passthrough;
}
sc->present_mode = STEREO_PRESENT_NV3DLIB;
sc->stereo_active = true;
sc->real_swapchain = VK_NULL_HANDLE;
*pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
sc->app_handle = *pSwapchain;
if (slot_is_new)
{
CHECK_ARRAY_COUNT(sd->swapchain_count, MAX_SWAPCHAINS, "swapchain_count");
sd->swapchain_count++;
}
if (sc->barrier_cmds)
{
}
if (sc->barrier_fences)
{
}
return VK_SUCCESS;
}
if (req == STEREO_PRESENT_AUTO || req == STEREO_PRESENT_DXGI) {
bool dxgi_ok = false;
HANDLE nt_handle = NULL;
if (sd->dxgi_init_in_progress) goto passthrough;
if (sc->hwnd && dxgi_device_init(sd)) {
dxgi_stereo_activate(sd);
sd->dxgi_init_in_progress = true;
if (dxgi_sc_create(sd, sc, &nt_handle)) {
dxgi_ok = true;
sd->dxgi_init_in_progress = false;
} else {
sd->dxgi_init_in_progress = false;
dxgi_sc_destroy(sc);
}
}
if (dxgi_ok) {
sc->image_count = 1;
sc->stereo_images = calloc(1, sizeof(VkImage));
sc->stereo_memory = calloc(1, sizeof(VkDeviceMemory));
sc->stereo_views_arr = calloc(1, sizeof(VkImageView));
sc->barrier_cmds = calloc(1, sizeof(VkCommandBuffer));
sc->barrier_fences = calloc(1, sizeof(VkFence));
if (!sc->stereo_images || !sc->stereo_memory || !sc->stereo_views_arr ||
!sc->barrier_cmds || !sc->barrier_fences) {
dxgi_sc_destroy(sc);
if (req == STEREO_PRESENT_DXGI) goto passthrough;
goto try_dx9;
}
VkResult res = alloc_external_stereo_image(sd, sc,
&sc->stereo_images[0], &sc->stereo_memory[0]);
if (res != VK_SUCCESS) {
dxgi_sc_destroy(sc);
if (req == STEREO_PRESENT_DXGI) goto passthrough;
goto try_dx9;
}
VkImageViewCreateInfo vci = {
.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
.image = sc->stereo_images[0],
.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
.format = sc->format,
.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 },
};
res = sd->real.CreateImageView(sd->real_device, &vci, NULL, &sc->stereo_views_arr[0]);
if (res != VK_SUCCESS) { dxgi_sc_destroy(sc); goto try_dx9; }
if (!setup_barrier_resources(sd, sc)) { dxgi_sc_destroy(sc); goto try_dx9; }
sc->present_mode = STEREO_PRESENT_DXGI;
sc->dxgi_mode = true;
sc->stereo_active = true;
sc->real_swapchain = VK_NULL_HANDLE;
*pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
sc->app_handle = *pSwapchain;
sd->stereo_w = app_w; sd->stereo_h = app_h;
if (slot_is_new)
{
CHECK_ARRAY_COUNT(sd->swapchain_count, MAX_SWAPCHAINS, "swapchain_count");
sd->swapchain_count++;
}
return VK_SUCCESS;
}
if (req == STEREO_PRESENT_DXGI) { STEREO_ERR("DXGI forced but failed"); goto passthrough; }
}
try_dx9:
if (req == STEREO_PRESENT_AUTO || req == STEREO_PRESENT_DX9) {
if (!sd->d3d11_ok) dxgi_device_init(sd);
if (sc->hwnd && dx9_init(sd, sc)) {
VkResult res = alloc_alt_stereo_swapchain(sd, sc);
if (res == VK_SUCCESS) res = alt_cpu_staging_init(sd, sc); 
if (res == VK_SUCCESS && setup_barrier_resources(sd, sc)) {
sc->present_mode = STEREO_PRESENT_DX9;
sc->dxgi_mode = false;
sc->stereo_active = true;
sc->real_swapchain = VK_NULL_HANDLE;
*pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
sc->app_handle = *pSwapchain;
sd->stereo_w = app_w; sd->stereo_h = app_h;
if (slot_is_new)
{
CHECK_ARRAY_COUNT(sd->swapchain_count, MAX_SWAPCHAINS, "swapchain_count");
sd->swapchain_count++;
}
return VK_SUCCESS;
}
}
if (req == STEREO_PRESENT_DX9) { STEREO_ERR("DX9 forced but failed"); goto passthrough; }
req = STEREO_PRESENT_SBS;
}
if (req == STEREO_PRESENT_SBS ||
req == STEREO_PRESENT_TAB ||
req == STEREO_PRESENT_INTERLACED) {
if (sc->hwnd && gpu_compose_sc_init(sd, sc, pCreateInfo->surface)) {
VkResult res = alloc_alt_stereo_swapchain(sd, sc);
if (res == VK_SUCCESS && setup_barrier_resources(sd, sc)) {
sc->present_mode = req;
sc->dxgi_mode = false;
sc->stereo_active = true;
*pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
sc->app_handle = *pSwapchain;
if (slot_is_new)
{
CHECK_ARRAY_COUNT(sd->swapchain_count, MAX_SWAPCHAINS, "swapchain_count");
sd->swapchain_count++;
}
sd->stereo_w = app_w;
sd->stereo_h = app_h;
return VK_SUCCESS;
}
gpu_compose_sc_destroy(sd, sc);
if (sc->real_swapchain) {
sd->real.DestroySwapchainKHR(sd->real_device, sc->real_swapchain, NULL);
sc->real_swapchain = VK_NULL_HANDLE;
}
}
}
passthrough:
STEREO_ERR("All stereo modes failed — passthrough");
sc->stereo_active = false;
VkSwapchainCreateInfoKHR ci = *pCreateInfo;
if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
{
StereoSwapchain *old_sc =
stereo_swapchain_lookup(sd, pCreateInfo->oldSwapchain);
if (old_sc)
{
ci.oldSwapchain = old_sc->real_swapchain;
}
else
{
ci.oldSwapchain = pCreateInfo->oldSwapchain;
}
}
VkResult res =
sd->real.CreateSwapchainKHR(
sd->real_device,
&ci,
pAllocator,
pSwapchain);
if (res == VK_SUCCESS) {
sc->real_swapchain = *pSwapchain;
sc->app_handle = *pSwapchain;
sc->stereo_active = false;
if (slot_is_new)
{
CHECK_ARRAY_COUNT(sd->swapchain_count, MAX_SWAPCHAINS, "swapchain_count");
sd->swapchain_count++;
}
}
return res;
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
const VkAllocationCallbacks *pAllocator)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return;
StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
if (sc && sc->resize_reused)
{
sc->resize_reused = false;
return;
}
if (sc) {
for (uint32_t i = 0; i < sc->image_count; i++)
{
if (sc->stereo_views_arr && sc->stereo_views_arr[i])
{
stereo_DestroyImageView(
device,
sc->stereo_views_arr[i],
NULL);
}
if (sc->stereo_images && sc->stereo_images[i])
{
bool depth_match = false;
bool color_match = false;
for (uint32_t d = 0; d < sd->intercepted_depth_count; d++)
{
if (sd->intercepted_depth[d] == sc->stereo_images[i])
{
depth_match = true;
break;
}
}
for (uint32_t c = 0; c < sd->intercepted_color_count; c++)
{
if (sd->intercepted_color[c] == sc->stereo_images[i])
{
color_match = true;
break;
}
}
remove_tracked_image(
sd->intercepted_depth,
&sd->intercepted_depth_count,
sc->stereo_images[i]);
remove_tracked_image(
sd->intercepted_color,
&sd->intercepted_color_count,
sc->stereo_images[i]);
tracked_destroy_image(
sd,
sc->stereo_images[i],
"swapchain stereo image");
}
if (sc->stereo_memory && sc->stereo_memory[i])
{
sd->real.FreeMemory(
sd->real_device,
sc->stereo_memory[i],
NULL);
}
if (sc->barrier_fences && sc->barrier_fences[i])
sd->real.DestroyFence(sd->real_device, sc->barrier_fences[i], NULL);
}
free(sc->stereo_views_arr);
free(sc->stereo_images);
free(sc->stereo_memory);
free(sc->barrier_cmds);
free(sc->barrier_fences);
sc->stereo_views_arr = NULL;
sc->stereo_images = NULL;
sc->stereo_memory = NULL;
sc->barrier_cmds = NULL;
sc->barrier_fences = NULL;
sc->image_count = 0;
if (sc->barrier_pool)
sd->real.DestroyCommandPool(sd->real_device, sc->barrier_pool, NULL);
sc->barrier_pool = VK_NULL_HANDLE;
if (sc->present_mode == STEREO_PRESENT_NV3DLIB)
nv3d_destroy(sd);
gpu_compose_sc_destroy(sd, sc); 
alt_cpu_staging_destroy(sd, sc); 
dxgi_sc_destroy(sc);
if (sc->real_swapchain)
{
sd->real.DestroySwapchainKHR(
sd->real_device,
sc->real_swapchain,
pAllocator);
sc->real_swapchain = VK_NULL_HANDLE;
}
sc->stereo_active = false;
sc->app_handle = VK_NULL_HANDLE;
} else {
sd->real.DestroySwapchainKHR(
sd->real_device,
swapchain,
pAllocator);
}
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_GetSwapchainImagesKHR(
VkDevice device,
VkSwapchainKHR swapchain,
uint32_t *pCount,
VkImage *pImages)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
if (!sc || !sc->stereo_active)
{
if (sc && sc->real_swapchain == VK_NULL_HANDLE)
{
return VK_ERROR_OUT_OF_DATE_KHR;
}
VkSwapchainKHR real =
sc ? sc->real_swapchain : swapchain;
return sd->real.GetSwapchainImagesKHR(
sd->real_device,
real,
pCount,
pImages);
}
if (!pImages)
{
*pCount = sc->image_count;
return VK_SUCCESS;
}
uint32_t copy = (*pCount < sc->image_count) ? *pCount : sc->image_count;
for (uint32_t i = 0; i < copy; i++)
{
pImages[i] = sc->stereo_images[i];
}
*pCount = copy;
return (copy < sc->image_count) ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
uint64_t timeout, VkSemaphore semaphore,
VkFence fence, uint32_t *pImageIndex)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
StereoSwapchain *sc = stereo_swapchain_lookup(sd, swapchain);
if (sc &&
sc->present_mode == STEREO_PRESENT_NV3DLIB)
{
if (semaphore != VK_NULL_HANDLE)
{
VkSubmitInfo sig = {
.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
.signalSemaphoreCount = 1, 
.pSignalSemaphores = &semaphore,
};
sd->real.QueueSubmit(
sd->gfx_queue,
1,
&sig,
VK_NULL_HANDLE);
}
if (fence != VK_NULL_HANDLE)
{
sd->real.ResetFences(
sd->real_device,
1,
&fence);
sd->real.QueueSubmit(
sd->gfx_queue,
0,
NULL,
fence);
}
if (pImageIndex)
*pImageIndex = 0;
return VK_SUCCESS;
}
if (!sc || !sc->stereo_active)
{
if (sc && sc->real_swapchain == VK_NULL_HANDLE)
{
STEREO_ERR(
"[ACQUIRE] called on destroyed stereo swapchain");
return VK_ERROR_OUT_OF_DATE_KHR;
}
VkSwapchainKHR real =
sc ? sc->real_swapchain : swapchain;
return sd->real.AcquireNextImageKHR(
sd->real_device,
real,
timeout,
semaphore,
fence,
pImageIndex);
}
if (sc->barrier_fences && sc->barrier_fences[0]) {
VkResult wres = sd->real.WaitForFences(
sd->real_device, 1, &sc->barrier_fences[0], VK_TRUE, timeout);
if (wres != VK_SUCCESS) return wres;
}
if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
VkSubmitInfo sig = {
.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
.signalSemaphoreCount = (semaphore != VK_NULL_HANDLE) ? 1 : 0,
.pSignalSemaphores = &semaphore,
};
if (sd->gfx_queue) sd->real.QueueSubmit(sd->gfx_queue, 1, &sig, fence);
}
*pImageIndex = 0;
return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
StereoDevice *sd = NULL;
StereoSwapchain *sc = NULL;
for (uint32_t d = 0; d < g_device_count && !sd; d++) {
for (uint32_t p = 0; p < pPresentInfo->swapchainCount; p++) {
StereoSwapchain *found = stereo_swapchain_lookup(
&g_devices[d], pPresentInfo->pSwapchains[p]);
if (found) { sd = &g_devices[d]; sc = found; break; }
}
}
if (!sd || !sc || !sd->stereo.enabled || !sc->stereo_active) {
StereoDevice *fwd = sd ? sd : (g_device_count > 0 ? &g_devices[0] : NULL);
if (!fwd) return VK_ERROR_DEVICE_LOST;
return fwd->real.QueuePresentKHR(queue, pPresentInfo);
}
hotkeys_poll(sd);
VkResult result = VK_SUCCESS;
for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
StereoSwapchain *sc_i = stereo_swapchain_lookup(sd, pPresentInfo->pSwapchains[i]);
if (!sc_i || !sc_i->stereo_active) continue;
uint32_t wcount = (i == 0) ? pPresentInfo->waitSemaphoreCount : 0;
const VkSemaphore *wsems = (i == 0) ? pPresentInfo->pWaitSemaphores : NULL;
VkResult pr;
switch (sc_i->present_mode) {
case STEREO_PRESENT_DXGI:
pr = stereo_dxgi_present(sd, queue, sc_i, 0, wcount, wsems);
break;
case STEREO_PRESENT_DX9:
pr = dx9_present(sd, sc_i, queue, wcount, wsems);
break;
case STEREO_PRESENT_NV3DLIB:
pr = nv3d_present(
sd,
sc_i,
queue,
wcount,
wsems);
break;
case STEREO_PRESENT_SBS:
case STEREO_PRESENT_TAB:
case STEREO_PRESENT_INTERLACED:
pr = gpu_compose_present(sd, sc_i, queue, wcount, wsems);
break;
default:
pr = VK_SUCCESS;
break;
}
if (pr == VK_SUBOPTIMAL_KHR) pr = VK_SUCCESS;
if (pr != VK_SUCCESS) {
STEREO_ERR("Present (mode=%d) failed: %d", (int)sc_i->present_mode, pr);
result = pr;
}
}
return result;
}
static bool is_depth_format(VkFormat fmt)
{
switch (fmt) {
case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT:
case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D16_UNORM_S8_UINT:
case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
return true;
default: return false;
}
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
static uint64_t image_create_seq = 0;
uint64_t seq = ++image_create_seq;
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
bool base = sd->stereo.enabled && sd->stereo.multiview
&& pCreateInfo
&& pCreateInfo->imageType == VK_IMAGE_TYPE_2D
&& pCreateInfo->arrayLayers == 1;
bool intercept_depth = base
&& (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
bool intercept_color = base
&& pCreateInfo->mipLevels == 1
&& pCreateInfo->extent.width > 1
&& pCreateInfo->extent.height > 1
&& (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
bool intercept_storage = base
&& pCreateInfo->mipLevels == 1
&& pCreateInfo->extent.width > 1
&& pCreateInfo->extent.height > 1
&& (pCreateInfo->usage & VK_IMAGE_USAGE_STORAGE_BIT);
if (base &&
(pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
!(pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
{
}
if (!intercept_depth && !intercept_color && !intercept_storage)
{
VkResult r =
sd->real.CreateImage(
sd->real_device,
pCreateInfo,
pAllocator,
pImage);
return r;
}
VkImageCreateInfo modified = *pCreateInfo;
modified.arrayLayers = 2;
VkResult res = sd->real.CreateImage(sd->real_device, &modified, pAllocator, pImage);
if (res == VK_SUCCESS) {
if (intercept_depth &&
sd->intercepted_depth_count < MAX_DEPTH_IMAGES)
{
bool already_tracked = false;
for (uint32_t i = 0;
i < sd->intercepted_depth_count;
i++)
{
if (sd->intercepted_depth[i] == *pImage)
{
already_tracked = true;
break;
}
}
if (!already_tracked)
{
if (sd->intercepted_depth_count < MAX_DEPTH_IMAGES)
{
sd->intercepted_depth[
sd->intercepted_depth_count++] = *pImage;
}
}
}
else if (intercept_depth)
{
}
if (intercept_color &&
sd->intercepted_color_count < MAX_COLOR_IMAGES)
{
bool already_tracked = false;
for (uint32_t i = 0;
i < sd->intercepted_color_count;
i++)
{
if (sd->intercepted_color[i] == *pImage)
{
already_tracked = true;
break;
}
}
if (!already_tracked)
{
if (sd->intercepted_color_count < MAX_COLOR_IMAGES)
{
sd->intercepted_color[
sd->intercepted_color_count++] = *pImage;
}
}
}
else if (intercept_color)
{
}
if (intercept_storage &&
sd->intercepted_storage_count < MAX_COLOR_IMAGES)
{
bool already_tracked = false;
for (uint32_t i = 0;
i < sd->intercepted_storage_count;
i++)
{
if (sd->intercepted_storage[i] == *pImage)
{
already_tracked = true;
break;
}
}
if (!already_tracked)
{
sd->intercepted_storage[
sd->intercepted_storage_count++] = *pImage;
}
}
}
return res;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
if (pCreateInfo->viewType == VK_IMAGE_VIEW_TYPE_CUBE ||
pCreateInfo->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
{
return sd->real.CreateImageView(
sd->real_device,
pCreateInfo,
pAllocator,
pView);
}
if (!sd->stereo.multiview)
return sd->real.CreateImageView(sd->real_device, pCreateInfo, pAllocator, pView);
bool needs_upgrade = false;
bool swapchain_match = false;
bool depth_match = false;
bool color_match = false;
bool storage_match = false;
for (uint32_t si = 0; si < sd->swapchain_count; si++) {
StereoSwapchain *scc = &sd->swapchains[si];
if (!scc->stereo_active || !scc->stereo_images) continue;
for (uint32_t ii = 0; ii < scc->image_count; ii++)
if (scc->stereo_images[ii] == pCreateInfo->image)
{
needs_upgrade = true;
swapchain_match = true;
}
}
uint32_t depth_matches = 0;
uint32_t color_matches = 0;
for (uint32_t i = 0; i < sd->intercepted_depth_count; i++)
{
if (sd->intercepted_depth[i] == pCreateInfo->image)
{
depth_matches++;
needs_upgrade = true;
swapchain_match = true;
}
}
for (uint32_t i = 0; i < sd->intercepted_color_count; i++)
{
if (sd->intercepted_color[i] == pCreateInfo->image)
{
color_matches++;
needs_upgrade = true;
swapchain_match = true;
}
}
for (uint32_t i = 0; i < sd->intercepted_storage_count; i++)
{
if (sd->intercepted_storage[i] == pCreateInfo->image)
{
storage_match = true;
needs_upgrade = true;
}
}
for (uint32_t i = 0; i < sd->upgraded_image_count; i++)
{
if (sd->upgraded_images[i] == pCreateInfo->image)
{
color_matches++;
needs_upgrade = true;
swapchain_match = true;
}
}
if (!needs_upgrade &&
(pCreateInfo->subresourceRange.aspectMask &
VK_IMAGE_ASPECT_DEPTH_BIT))
{
}
if (!needs_upgrade)
{
VkResult r =
sd->real.CreateImageView(
sd->real_device,
pCreateInfo,
pAllocator,
pView);
return r;
}
VkImageViewCreateInfo upgraded = *pCreateInfo;
if (upgraded.viewType == VK_IMAGE_VIEW_TYPE_2D)
upgraded.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
if (upgraded.subresourceRange.layerCount < 2)
upgraded.subresourceRange.layerCount = 2;
VkResult _r=
sd->real.CreateImageView(
sd->real_device,
&upgraded,
pAllocator,
pView);
if (_r == VK_SUCCESS)
{
}
if (_r == VK_SUCCESS &&
sd->upgraded_view_count < MAX_UPGRADED_VIEWS)
{
if (sd->upgraded_view_count < MAX_UPGRADED_VIEWS)
{
CHECK_ARRAY_COUNT(sd->upgraded_view_count, MAX_UPGRADED_VIEWS, "upgraded_view_count");
sd->upgraded_views[sd->upgraded_view_count++] = *pView;
}
if (sd->upgraded_image_count < MAX_UPGRADED_VIEWS)
{
CHECK_ARRAY_COUNT(sd->upgraded_image_count, MAX_UPGRADED_VIEWS, "upgraded_image_count");
sd->upgraded_images[sd->upgraded_image_count++] =
pCreateInfo->image;
}
}
return _r;
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyImageView(
VkDevice device,
VkImageView imageView,
const VkAllocationCallbacks *pAllocator)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd)
return;
for (uint32_t i = 0;
i < sd->upgraded_view_count;
i++)
{
if (sd->upgraded_views[i] == imageView)
{
uint32_t last = --sd->upgraded_view_count;
if (i != last)
sd->upgraded_views[i] = sd->upgraded_views[last];
sd->upgraded_views[last] = VK_NULL_HANDLE;
break;
}
}
sd->real.DestroyImageView(
sd->real_device,
imageView,
pAllocator);
}