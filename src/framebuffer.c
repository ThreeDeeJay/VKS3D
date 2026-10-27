
#include <string.h>
#include "stereo_icd.h"
StereoDevice* stereo_device_from_command_buffer(VkCommandBuffer cb);
void remember_begin_renderpass(
StereoDevice* sd,
VkCommandBuffer cb,
VkRenderPass rp,
uint32_t flags);
StereoDevice *
stereo_device_from_command_buffer(
VkCommandBuffer cb);
VkRenderPass lookup_bound_renderpass(
StereoDevice* sd,
VkCommandBuffer cb);
VkFramebuffer lookup_bound_framebuffer(
StereoDevice* sd,
VkCommandBuffer cb);
VkPipeline lookup_bound_pipeline(
StereoDevice* sd,
VkCommandBuffer cb);
static void stereo_overwrite_projection_binding(
StereoDevice *sd,
VkDescriptorSet ds,
uint32_t binding);
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateFramebuffer(
VkDevice device,
const VkFramebufferCreateInfo *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkFramebuffer *pFramebuffer)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
VkFramebufferCreateInfo fci = *pCreateInfo;
VkRenderPass debug_original = pCreateInfo->renderPass;
for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
}
if (debug_original == VK_NULL_HANDLE) {
}
VkRenderPass original_rp = pCreateInfo->renderPass;
VkRenderPass use_mv = VK_NULL_HANDLE;
StereoRenderPassInfo *fb_rpi = stereo_rp_lookup(sd, original_rp);
if (original_rp == VK_NULL_HANDLE) {
}
if (sd->stereo.enabled && sd->stereo.multiview && pCreateInfo->attachmentCount > 0) {
bool all = true;
bool any = false;
bool only_shading_rate_non_upgraded = true;
StereoRenderPassInfo *rpi =
stereo_rp_lookup(sd, pCreateInfo->renderPass);
uint32_t shading_rate_attachment =
rpi ? rpi->shading_rate_attachment : VK_ATTACHMENT_UNUSED;
bool has_shading_rate_attachment =
rpi ? rpi->has_shading_rate_attachment : false;
for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) 
{
VkImageView view = pCreateInfo->pAttachments[i];
bool found = false;
uint32_t found_index = UINT32_MAX;
for (uint32_t k = 0; k < sd->upgraded_view_count; k++) {
if (sd->upgraded_views[k] == view) {
found = true;
found_index = k;
break;
}
}
if (found) {
any = true;
} else {
all = false;
if (!(has_shading_rate_attachment &&
shading_rate_attachment == i))
{
only_shading_rate_non_upgraded = false;
}
}
}
bool mv_attachment_eligible =
all ||
(
has_shading_rate_attachment &&
any &&
only_shading_rate_non_upgraded
);
if (rpi &&
mv_attachment_eligible &&
rpi->mv_handle &&
rpi->has_multiview &&
rpi->handle == pCreateInfo->renderPass)
{
fci.renderPass = rpi->mv_handle;
use_mv = rpi->mv_handle;
}
else
{
}
if (!all) 
{
for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
{
bool found = false;
for (uint32_t k = 0; k < sd->upgraded_view_count; k++) 
{
if (sd->upgraded_views[k] ==
pCreateInfo->pAttachments[i])
{
found = true;
break;
}
}
if (!found)
{
}
}
}
}
if (fci.renderPass == VK_NULL_HANDLE && use_mv != VK_NULL_HANDLE) {
}
VkRenderPass before = fci.renderPass;
VkResult res = sd->real.CreateFramebuffer(sd->real_device, &fci, pAllocator, pFramebuffer);
if (res == VK_SUCCESS)
{
}
if (before != fci.renderPass) {
}
if (fci.renderPass == VK_NULL_HANDLE) {
}
if (pCreateInfo->renderPass == VK_NULL_HANDLE) {
}
if (res == VK_SUCCESS)
{
StereoFramebufferTrack *t;
uint32_t idx;
stereo_mutex_lock(&sd->lock);
if (sd->fb_track_count >= MAX_FB_TRACK)
{
uint32_t count = sd->fb_track_count;
stereo_mutex_unlock(&sd->lock);
return res;
}
CHECK_ARRAY_COUNT(sd->fb_track_count, MAX_FB_TRACK, "fb_track_count");
idx = sd->fb_track_count++;
t = &sd->fb_tracks[idx];
memset(t, 0, sizeof(*t));
t->fb = *pFramebuffer;
t->rp = original_rp;
if (t->rp == VK_NULL_HANDLE)
t->rp = fci.renderPass;
t->rp_used_at_create = fci.renderPass;
t->mv_rp = use_mv;
t->has_mv = (use_mv != VK_NULL_HANDLE) &&
sd->stereo.multiview;
if (sd->stereo.enabled && sd->stereo.multiview && use_mv == VK_NULL_HANDLE)
{
}
if (use_mv != VK_NULL_HANDLE &&
fci.renderPass == VK_NULL_HANDLE) 
{
}
if (use_mv != VK_NULL_HANDLE &&
!sd->stereo.multiview)
{
}
if (use_mv == VK_NULL_HANDLE) 
{
}
StereoFramebufferTrack *verify = &sd->fb_tracks[idx];
stereo_mutex_unlock(&sd->lock);
}
return res;
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyFramebuffer(
VkDevice device,
VkFramebuffer framebuffer,
const VkAllocationCallbacks *pAllocator)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return;
stereo_mutex_lock(&sd->lock);
for (uint32_t i = 0; i < sd->fb_track_count; i++)
{
if (sd->fb_tracks[i].fb == framebuffer)
{
uint32_t last = --sd->fb_track_count;
if (i != last)
sd->fb_tracks[i] = sd->fb_tracks[last];
memset(&sd->fb_tracks[last], 0, sizeof(sd->fb_tracks[last]));
break;
}
}
stereo_mutex_unlock(&sd->lock);
sd->real.DestroyFramebuffer(
sd->real_device,
framebuffer,
pAllocator);
}
static StereoDevice *
find_any_device(void)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
for (uint32_t i = 0; i < g_device_count; i++)
{
if (g_devices[i].real_device)
return &g_devices[i];
}
return NULL;
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdBeginRenderPass(
VkCommandBuffer commandBuffer,
const VkRenderPassBeginInfo *pRenderPassBegin,
VkSubpassContents contents)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
StereoDevice *sd = NULL;
VkRenderPass mv_rp = VK_NULL_HANDLE;
bool fb_found = false;
bool fb_has_mv = false;
uint32_t fb_track_index = UINT32_MAX;
for (uint32_t d = 0; d < g_device_count; d++)
{
StereoDevice *dev = &g_devices[d];
}
for (uint32_t d = 0; d < g_device_count && !sd; d++) {
StereoDevice *dev = &g_devices[d];
for (uint32_t i = 0; i < dev->fb_track_count; i++) {
bool fb_match =
(dev->fb_tracks[i].fb == pRenderPassBegin->framebuffer);
if (fb_match)
{
bool rp_match =
(
dev->fb_tracks[i].rp &&
pRenderPassBegin->renderPass &&
dev->fb_tracks[i].rp == pRenderPassBegin->renderPass
)
||
(
dev->fb_tracks[i].mv_rp &&
pRenderPassBegin->renderPass &&
dev->fb_tracks[i].mv_rp == pRenderPassBegin->renderPass
);
fb_found = true;
sd = dev;
fb_track_index = i;
fb_has_mv =
dev->fb_tracks[i].has_mv &&
dev->fb_tracks[i].mv_rp != VK_NULL_HANDLE;
if (fb_has_mv)
{
mv_rp = dev->fb_tracks[i].mv_rp;
}
else
{
}
break;
}
}
}
if (!sd)
{
for (uint32_t d = 0; d < g_device_count; d++)
{
if (g_devices[d].real_device)
{
sd = &g_devices[d];
break;
}
}
}
if (!sd) return;
StereoRenderPassInfo *lookup =
stereo_rp_lookup(sd, pRenderPassBegin->renderPass);
if (fb_found)
{
VkRenderPass tracked_rp = sd->fb_tracks[fb_track_index].rp;
VkRenderPass tracked_mv_rp = sd->fb_tracks[fb_track_index].mv_rp;
bool tracked_rp_match =
tracked_rp &&
pRenderPassBegin->renderPass &&
tracked_rp == pRenderPassBegin->renderPass;
if (fb_has_mv && tracked_rp_match)
{
mv_rp = tracked_mv_rp;
}
else if (fb_has_mv)
{
if (lookup &&
lookup->has_multiview &&
lookup->mv_handle != VK_NULL_HANDLE)
{
mv_rp = lookup->mv_handle;
}
else
{
mv_rp = VK_NULL_HANDLE;
}
}
else
{
mv_rp = VK_NULL_HANDLE;
}
}
else if (lookup &&
lookup->has_multiview &&
lookup->mv_handle != VK_NULL_HANDLE)
{
mv_rp = lookup->mv_handle;
}
if (mv_rp == VK_NULL_HANDLE)
{
}
if (mv_rp)
{
VkRenderPassBeginInfo modified = *pRenderPassBegin;
modified.renderPass = mv_rp;
for (uint32_t i = 0; i < sd->fb_track_count; i++)
{
if (sd->fb_tracks[i].fb == modified.framebuffer)
{
break;
}
}
bool cb_found = false;
for (uint32_t i = 0; i < sd->cb_track_count; i++)
{
if (sd->cb_track[i].cb == commandBuffer)
{
sd->cb_track[i].render_pass = modified.renderPass;
sd->cb_track[i].framebuffer = modified.framebuffer;
cb_found = true;
break;
}
}
if (!cb_found && sd->cb_track_count < MAX_CB_TRACK)
{
uint32_t idx = sd->cb_track_count++;
sd->cb_track[idx].cb = commandBuffer;
sd->cb_track[idx].render_pass = modified.renderPass;
sd->cb_track[idx].framebuffer = modified.framebuffer;
}
else if (!cb_found)
{
}
sd->real.CmdBeginRenderPass(commandBuffer, &modified, contents);
} else {
for (uint32_t i = 0; i < sd->cb_track_count; i++)
{
if (sd->cb_track[i].cb == commandBuffer)
{
sd->cb_track[i].render_pass = pRenderPassBegin->renderPass;
sd->cb_track[i].framebuffer = pRenderPassBegin->framebuffer;
break;
}
}
sd->real.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
StereoDevice *sd = find_any_device();
if (!sd) return;
for (uint32_t i = 0; i < sd->cb_track_count; i++)
{
if (sd->cb_track[i].cb == commandBuffer)
{
sd->cb_track[i].render_pass = VK_NULL_HANDLE;
sd->cb_track[i].framebuffer = VK_NULL_HANDLE;
break;
}
}
sd->real.CmdEndRenderPass(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdBeginRendering(
VkCommandBuffer commandBuffer,
const VkRenderingInfo *pRenderingInfo)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
StereoDevice *sd = NULL;
for (uint32_t i = 0; i < g_device_count; i++)
{
if (g_devices[i].real_device)
{
sd = &g_devices[i];
break;
}
}
if (!sd)
{
return;
}
if (!sd->real.CmdBeginRendering)
{
return;
}
VkRenderingInfo modified = *pRenderingInfo;
if (sd->stereo.multiview &&
!sd->stereo.shader_objects_mono &&
modified.viewMask == 0)
{
modified.viewMask = 0x3;
}
sd->real.CmdBeginRendering(
commandBuffer,
&modified);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdEndRendering(
VkCommandBuffer commandBuffer)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
StereoDevice *sd = NULL;
for (uint32_t i = 0; i < g_device_count; i++)
{
if (g_devices[i].real_device)
{
sd = &g_devices[i];
break;
}
}
if (!sd || !sd->real.CmdEndRendering)
return;
sd->real.CmdEndRendering(commandBuffer);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindPipeline(
VkCommandBuffer commandBuffer,
VkPipelineBindPoint pipelineBindPoint,
VkPipeline pipeline)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
StereoDevice *sd = NULL;
VkRenderPass active_rp = VK_NULL_HANDLE;
VkFramebuffer active_fb = VK_NULL_HANDLE;
for (uint32_t i = 0; i < g_device_count; i++)
{
if (g_devices[i].real_device)
{
sd = &g_devices[i];
break;
}
}
if (!sd)
return;
for (uint32_t i = 0; i < sd->cb_track_count; i++)
{
if (sd->cb_track[i].cb == commandBuffer)
{
active_rp = sd->cb_track[i].render_pass;
active_fb = sd->cb_track[i].framebuffer;
break;
}
}
StereoPipelineInfo *info =
find_pipeline_info(sd, pipeline);
remember_bound_pipeline(
sd,
commandBuffer,
pipeline);
sd->real.CmdBindPipeline(
commandBuffer,
pipelineBindPoint,
pipeline);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdTraceRaysKHR(
VkCommandBuffer commandBuffer,
const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
uint32_t width,
uint32_t height,
uint32_t depth)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
StereoDevice *sd = NULL;
for (uint32_t i = 0; i < g_device_count; i++)
{
if (g_devices[i].real_device)
{
sd = &g_devices[i];
break;
}
}
if (!sd || !sd->real.CmdTraceRaysKHR)
{
STEREO_ERR(
"RT_TRACE missing dispatch sd=%p real=%p",
(void*)sd,
sd ? (void*)sd->real.CmdTraceRaysKHR : NULL);
return;
}
if (pRaygenShaderBindingTable)
{
}
if (pMissShaderBindingTable)
{
}
if (pHitShaderBindingTable)
{
}
if (pCallableShaderBindingTable)
{
}
uint32_t rt_depth = depth;
if (sd->stereo.enabled && depth == 1)
rt_depth = 2;
sd->real.CmdTraceRaysKHR(
commandBuffer,
pRaygenShaderBindingTable,
pMissShaderBindingTable,
pHitShaderBindingTable,
pCallableShaderBindingTable,
width,
height,
rt_depth);
if (!sd->stereo.enabled || rt_depth < 2)
{
return;
}
VkImage rt_image = VK_NULL_HANDLE;
if (sd->intercepted_storage_count > 0)
rt_image = sd->intercepted_storage[sd->intercepted_storage_count - 1];
if (rt_image == VK_NULL_HANDLE)
{
return;
}
StereoSwapchain *sc = NULL;
for (uint32_t si = 0; si < sd->swapchain_count; si++)
{
if (sd->swapchains[si].stereo_active &&
sd->swapchains[si].stereo_images &&
sd->swapchains[si].image_count > 0)
{
sc = &sd->swapchains[si];
break;
}
}
if (!sc)
{
return;
}
VkImage dst_image = sc->stereo_images[0];
VkImageMemoryBarrier src_barrier = {
.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.image = rt_image,
.subresourceRange = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.baseMipLevel = 0,
.levelCount = 1,
.baseArrayLayer = 0,
.layerCount = 2
}
};
sd->real.CmdPipelineBarrier(
commandBuffer,
VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
VK_PIPELINE_STAGE_TRANSFER_BIT,
0,
0,
NULL,
0,
NULL,
1,
&src_barrier);
VkImageMemoryBarrier dst_barrier = {
.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.image = dst_image,
.subresourceRange = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.baseMipLevel = 0,
.levelCount = 1,
.baseArrayLayer = 0,
.layerCount = 2
}
};
sd->real.CmdPipelineBarrier(
commandBuffer,
VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
VK_PIPELINE_STAGE_TRANSFER_BIT,
0,
0,
NULL,
0,
NULL,
1,
&dst_barrier);
VkImageCopy copy = {
.srcSubresource = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.mipLevel = 0,
.baseArrayLayer = 0,
.layerCount = 2
},
.srcOffset = { 0, 0, 0 },
.dstSubresource = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.mipLevel = 0,
.baseArrayLayer = 0,
.layerCount = 2
},
.dstOffset = { 0, 0, 0 },
.extent = { width, height, 1 }
};
sd->real.CmdCopyImage(
commandBuffer,
rt_image,
VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
dst_image,
VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
1,
&copy);
VkImageMemoryBarrier src_restore = {
.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
.newLayout = VK_IMAGE_LAYOUT_GENERAL,
.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.image = rt_image,
.subresourceRange = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.baseMipLevel = 0,
.levelCount = 1,
.baseArrayLayer = 0,
.layerCount = 2
}
};
sd->real.CmdPipelineBarrier(
commandBuffer,
VK_PIPELINE_STAGE_TRANSFER_BIT,
VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
0,
0,
NULL,
0,
NULL,
1,
&src_restore);
VkImageMemoryBarrier dst_restore = {
.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.image = dst_image,
.subresourceRange = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.baseMipLevel = 0,
.levelCount = 1,
.baseArrayLayer = 0,
.layerCount = 2
}
};
sd->real.CmdPipelineBarrier(
commandBuffer,
VK_PIPELINE_STAGE_TRANSFER_BIT,
VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
0,
0,
NULL,
0,
NULL,
1,
&dst_restore);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdDrawIndexed(
VkCommandBuffer commandBuffer,
uint32_t indexCount,
uint32_t instanceCount,
uint32_t firstIndex,
int32_t vertexOffset,
uint32_t firstInstance)
{
StereoDevice *sd = find_any_device();
if (!sd)
return;
VkPipeline pipe =
lookup_bound_pipeline(sd, commandBuffer);
VkRenderPass rp =
lookup_bound_renderpass(sd, commandBuffer);
VkFramebuffer fb =
lookup_bound_framebuffer(sd, commandBuffer);
StereoPipelineInfo *info =
find_pipeline_info(sd, pipe);
if (info)
{
}
else
{
}
uint32_t rp_view_mask = 0;
uint32_t rp_has_multiview = 0;
for (uint32_t i = 0; i < sd->render_pass_count; i++)
{
StereoRenderPassInfo *rpi = &sd->render_passes[i];
if (rpi->handle == rp || rpi->mv_handle == rp)
{
rp_view_mask = rpi->view_mask;
rp_has_multiview = rpi->has_multiview;
break;
}
}
sd->real.CmdDrawIndexed(
commandBuffer,
indexCount,
instanceCount,
firstIndex,
vertexOffset,
firstInstance);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdDraw(
VkCommandBuffer commandBuffer,
uint32_t vertexCount,
uint32_t instanceCount,
uint32_t firstVertex,
uint32_t firstInstance)
{
StereoDevice *sd = find_any_device();
if (!sd)
return;
VkPipeline pipe =
lookup_bound_pipeline(sd, commandBuffer);
VkRenderPass rp =
lookup_bound_renderpass(sd, commandBuffer);
VkFramebuffer fb =
lookup_bound_framebuffer(sd, commandBuffer);
StereoPipelineInfo *info =
find_pipeline_info(sd, pipe);
if (info)
{
}
else
{
}
uint32_t rp_view_mask = 0;
uint32_t rp_has_multiview = 0;
for (uint32_t i = 0; i < sd->render_pass_count; i++)
{
StereoRenderPassInfo *rpi = &sd->render_passes[i];
if (rpi->handle == rp || rpi->mv_handle == rp)
{
rp_view_mask = rpi->view_mask;
rp_has_multiview = rpi->has_multiview;
break;
}
}
sd->real.CmdDraw(
commandBuffer,
vertexCount,
instanceCount,
firstVertex,
firstInstance);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdDrawIndirect(
VkCommandBuffer commandBuffer,
VkBuffer buffer,
VkDeviceSize offset,
uint32_t drawCount,
uint32_t stride)
{
StereoDevice *sd = find_any_device();
if (!sd)
return;
VkPipeline pipe =
lookup_bound_pipeline(sd, commandBuffer);
VkRenderPass rp =
lookup_bound_renderpass(sd, commandBuffer);
VkFramebuffer fb =
lookup_bound_framebuffer(sd, commandBuffer);
StereoPipelineInfo *info =
find_pipeline_info(sd, pipe);
if (info)
{
}
else
{
}
uint32_t rp_view_mask = 0;
uint32_t rp_has_multiview = 0;
for (uint32_t i = 0; i < sd->render_pass_count; i++)
{
StereoRenderPassInfo *rpi = &sd->render_passes[i];
if (rpi->handle == rp || rpi->mv_handle == rp)
{
rp_view_mask = rpi->view_mask;
rp_has_multiview = rpi->has_multiview;
break;
}
}
sd->real.CmdDrawIndirect(
commandBuffer,
buffer,
offset,
drawCount,
stride);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdDrawIndexedIndirect(
VkCommandBuffer commandBuffer,
VkBuffer buffer,
VkDeviceSize offset,
uint32_t drawCount,
uint32_t stride)
{
StereoDevice *sd = find_any_device();
if (!sd)
return;
VkPipeline pipe =
lookup_bound_pipeline(sd, commandBuffer);
VkRenderPass rp =
lookup_bound_renderpass(sd, commandBuffer);
VkFramebuffer fb =
lookup_bound_framebuffer(sd, commandBuffer);
StereoPipelineInfo *info =
find_pipeline_info(sd, pipe);
if (info)
{
}
else
{
}
uint32_t rp_view_mask = 0;
uint32_t rp_has_multiview = 0;
for (uint32_t i = 0; i < sd->render_pass_count; i++)
{
StereoRenderPassInfo *rpi = &sd->render_passes[i];
if (rpi->handle == rp || rpi->mv_handle == rp)
{
rp_view_mask = rpi->view_mask;
rp_has_multiview = rpi->has_multiview;
break;
}
}
sd->real.CmdDrawIndexedIndirect(
commandBuffer,
buffer,
offset,
drawCount,
stride);
}
static void stereo_overwrite_projection_binding(
StereoDevice *sd,
VkDescriptorSet set,
uint32_t binding)
{
if (!sd || !sd->stereo.enabled || !sd->stereo_ubo_map || set == VK_NULL_HANDLE)
return;
StereoUBO *ubo = (StereoUBO *)sd->stereo_ubo_map;
if (!ubo)
return;
VkDescriptorBufferInfo bi = {
.buffer = sd->stereo_ubo,
.offset = 0,
.range = sizeof(StereoUBO),
};
VkWriteDescriptorSet w = {
.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
.dstSet = set,
.dstBinding = binding,
.dstArrayElement = 0,
.descriptorCount = 1,
.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
.pBufferInfo = &bi,
};
sd->real.UpdateDescriptorSets(sd->real_device, 1, &w, 0, NULL);
}
VKAPI_ATTR void VKAPI_CALL
stereo_UpdateDescriptorSets(
VkDevice device,
uint32_t descriptorWriteCount,
const VkWriteDescriptorSet *pDescriptorWrites,
uint32_t descriptorCopyCount,
const VkCopyDescriptorSet *pDescriptorCopies)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd)
return;
for (uint32_t i = 0; i < descriptorWriteCount; i++)
{
const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
{
if (!w->pBufferInfo)
continue;
for (uint32_t j = 0; j < w->descriptorCount; j++)
{
}
continue;
}
if (!w->pImageInfo)
continue;
if (w->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
w->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
w->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
continue;
for (uint32_t j = 0; j < w->descriptorCount; j++)
{
VkImageView view = w->pImageInfo[j].imageView;
bool upgraded = false;
for (uint32_t k = 0;
k < sd->upgraded_view_count;
k++)
{
if (sd->upgraded_views[k] == view)
{
upgraded = true;
break;
}
}
if (upgraded)
{
}
}
}
sd->real.UpdateDescriptorSets(
sd->real_device,
descriptorWriteCount,
pDescriptorWrites,
descriptorCopyCount,
pDescriptorCopies);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindDescriptorSets(
VkCommandBuffer commandBuffer,
VkPipelineBindPoint pipelineBindPoint,
VkPipelineLayout layout,
uint32_t firstSet,
uint32_t descriptorSetCount,
const VkDescriptorSet *pDescriptorSets,
uint32_t dynamicOffsetCount,
const uint32_t *pDynamicOffsets)
{
for (uint32_t i = 0; i < descriptorSetCount; i++)
{
}
StereoDevice *sd = find_any_device();
if (!sd)
return;
if (sd->stereo.enabled &&
pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS &&
pDescriptorSets &&
descriptorSetCount > 0)
{
VkPipeline pipe = lookup_bound_pipeline(sd, commandBuffer);
StereoPipelineInfo *info = find_pipeline_info(sd, pipe);
if (info &&
info->has_proj_ubo &&
info->proj_set != UINT32_MAX &&
info->proj_binding != UINT32_MAX &&
info->proj_member_mask != UINT32_MAX &&
info->proj_var != UINT32_MAX)
{
uint32_t target_set = info->proj_set;
if (target_set >= firstSet &&
target_set < firstSet + descriptorSetCount)
{
uint32_t rel = target_set - firstSet;
VkDescriptorSet ds = pDescriptorSets[rel];
if (ds != VK_NULL_HANDLE)
{
if (dynamicOffsetCount > 0 && pDynamicOffsets)
{
}
bool rewrite_proj = false;
if (rewrite_proj)
{
stereo_write_ubo(sd);
stereo_overwrite_projection_binding(
sd,
ds,
info->proj_binding);
}
else
{
}
}
}
}
else if (info)
{
}
}
sd->real.CmdBindDescriptorSets(
commandBuffer,
pipelineBindPoint,
layout,
firstSet,
descriptorSetCount,
pDescriptorSets,
dynamicOffsetCount,
pDynamicOffsets);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdBindShadersEXT(
VkCommandBuffer commandBuffer,
uint32_t stageCount,
const VkShaderStageFlagBits *pStages,
const VkShaderEXT *pShaders)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdBindShadersEXT)
return;
for (uint32_t i = 0; i < stageCount; i++)
{
}
sd->real.CmdBindShadersEXT(
commandBuffer,
stageCount,
pStages,
pShaders);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetViewport(
VkCommandBuffer commandBuffer,
uint32_t firstViewport,
uint32_t viewportCount,
const VkViewport *pViewports)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetViewport)
return;
sd->real.CmdSetViewport(
commandBuffer,
firstViewport,
viewportCount,
pViewports);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetScissor(
VkCommandBuffer commandBuffer,
uint32_t firstScissor,
uint32_t scissorCount,
const VkRect2D *pScissors)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetScissor)
return;
sd->real.CmdSetScissor(
commandBuffer,
firstScissor,
scissorCount,
pScissors);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetCullMode(
VkCommandBuffer commandBuffer,
VkCullModeFlags cullMode)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetCullMode)
return;
sd->real.CmdSetCullMode(commandBuffer, cullMode);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetFrontFace(
VkCommandBuffer commandBuffer,
VkFrontFace frontFace)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetFrontFace)
return;
sd->real.CmdSetFrontFace(commandBuffer, frontFace);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetPrimitiveTopology(
VkCommandBuffer commandBuffer,
VkPrimitiveTopology primitiveTopology)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetPrimitiveTopology)
return;
sd->real.CmdSetPrimitiveTopology(commandBuffer, primitiveTopology);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetViewportWithCountEXT(
VkCommandBuffer commandBuffer,
uint32_t viewportCount,
const VkViewport *pViewports)
{
for (uint32_t i = 0; i < viewportCount; i++)
{
}
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetViewportWithCountEXT)
return;
sd->real.CmdSetViewportWithCountEXT(
commandBuffer,
viewportCount,
pViewports);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetScissorWithCountEXT(
VkCommandBuffer commandBuffer,
uint32_t scissorCount,
const VkRect2D *pScissors)
{
for (uint32_t i = 0; i < scissorCount; i++)
{
}
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetScissorWithCountEXT)
return;
sd->real.CmdSetScissorWithCountEXT(
commandBuffer,
scissorCount,
pScissors);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthTestEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 depthTestEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetDepthTestEnableEXT)
return;
sd->real.CmdSetDepthTestEnableEXT(
commandBuffer,
depthTestEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthWriteEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 depthWriteEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetDepthWriteEnableEXT)
return;
sd->real.CmdSetDepthWriteEnableEXT(
commandBuffer,
depthWriteEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthCompareOpEXT(
VkCommandBuffer commandBuffer,
VkCompareOp depthCompareOp)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetDepthCompareOpEXT)
return;
sd->real.CmdSetDepthCompareOpEXT(
commandBuffer,
depthCompareOp);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetRasterizerDiscardEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 rasterizerDiscardEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetRasterizerDiscardEnableEXT)
return;
sd->real.CmdSetRasterizerDiscardEnableEXT(
commandBuffer,
rasterizerDiscardEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetPolygonModeEXT(
VkCommandBuffer commandBuffer,
VkPolygonMode polygonMode)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetPolygonModeEXT)
return;
sd->real.CmdSetPolygonModeEXT(
commandBuffer,
polygonMode);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetRasterizationSamplesEXT(
VkCommandBuffer commandBuffer,
VkSampleCountFlagBits rasterizationSamples)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetRasterizationSamplesEXT)
return;
sd->real.CmdSetRasterizationSamplesEXT(
commandBuffer,
rasterizationSamples);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetAlphaToCoverageEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 alphaToCoverageEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetAlphaToCoverageEnableEXT)
return;
sd->real.CmdSetAlphaToCoverageEnableEXT(
commandBuffer,
alphaToCoverageEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetDepthBiasEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 depthBiasEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetDepthBiasEnableEXT)
return;
sd->real.CmdSetDepthBiasEnableEXT(
commandBuffer,
depthBiasEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetStencilTestEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 stencilTestEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetStencilTestEnableEXT)
return;
sd->real.CmdSetStencilTestEnableEXT(
commandBuffer,
stencilTestEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetPrimitiveRestartEnableEXT(
VkCommandBuffer commandBuffer,
VkBool32 primitiveRestartEnable)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetPrimitiveRestartEnableEXT)
return;
sd->real.CmdSetPrimitiveRestartEnableEXT(
commandBuffer,
primitiveRestartEnable);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetSampleMaskEXT(
VkCommandBuffer commandBuffer,
VkSampleCountFlagBits samples,
const VkSampleMask *pSampleMask)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetSampleMaskEXT)
return;
sd->real.CmdSetSampleMaskEXT(
commandBuffer,
samples,
pSampleMask);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetColorBlendEnableEXT(
VkCommandBuffer commandBuffer,
uint32_t firstAttachment,
uint32_t attachmentCount,
const VkBool32 *pColorBlendEnables)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetColorBlendEnableEXT)
return;
sd->real.CmdSetColorBlendEnableEXT(
commandBuffer,
firstAttachment,
attachmentCount,
pColorBlendEnables);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetColorWriteMaskEXT(
VkCommandBuffer commandBuffer,
uint32_t firstAttachment,
uint32_t attachmentCount,
const VkColorComponentFlags *pColorWriteMasks)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetColorWriteMaskEXT)
return;
sd->real.CmdSetColorWriteMaskEXT(
commandBuffer,
firstAttachment,
attachmentCount,
pColorWriteMasks);
}
VKAPI_ATTR void VKAPI_CALL
stereo_CmdSetVertexInputEXT(
VkCommandBuffer commandBuffer,
uint32_t vertexBindingDescriptionCount,
const VkVertexInputBindingDescription2EXT *pVertexBindingDescriptions,
uint32_t vertexAttributeDescriptionCount,
const VkVertexInputAttributeDescription2EXT *pVertexAttributeDescriptions)
{
StereoDevice *sd = find_any_device();
if (!sd || !sd->real.CmdSetVertexInputEXT)
return;
sd->real.CmdSetVertexInputEXT(
commandBuffer,
vertexBindingDescriptionCount,
pVertexBindingDescriptions,
vertexAttributeDescriptionCount,
pVertexAttributeDescriptions);
}