
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "dxgi_output.h"
static void cmd_image_barrier(
StereoDevice *sd,
VkCommandBuffer cmd,
VkImage image,
uint32_t layer_count,
VkImageLayout old_layout,
VkImageLayout new_layout,
VkAccessFlags src_access,
VkAccessFlags dst_access,
VkPipelineStageFlags src_stage,
VkPipelineStageFlags dst_stage)
{
VkImageMemoryBarrier imb = {
.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
.srcAccessMask = src_access,
.dstAccessMask = dst_access,
.oldLayout = old_layout,
.newLayout = new_layout,
.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
.image = image,
.subresourceRange = {
.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
.baseMipLevel = 0, .levelCount = 1,
.baseArrayLayer = 0, .layerCount = layer_count,
},
};
sd->real.CmdPipelineBarrier(cmd,
src_stage, dst_stage,
0, 0, NULL, 0, NULL, 1, &imb);
}
static VkResult record_barrier_cmd(
StereoDevice *sd,
StereoSwapchain *sc)
{
VkCommandBuffer cmd = sc->barrier_cmds[0];
VkCommandBufferBeginInfo begin = {
.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
};
VkResult res = sd->real.BeginCommandBuffer(cmd, &begin);
if (res != VK_SUCCESS) return res;
cmd_image_barrier(sd, cmd, sc->stereo_images[0],
2, 
VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
VK_IMAGE_LAYOUT_GENERAL,
VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
VK_ACCESS_MEMORY_READ_BIT,
VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
return sd->real.EndCommandBuffer(cmd);
}
VkResult stereo_dxgi_present(
StereoDevice *sd,
VkQueue queue,
StereoSwapchain *sc,
uint32_t image_index, 
uint32_t wait_sem_count,
const VkSemaphore *wait_sems)
{
(void)image_index; 
if (!sc->stereo_active || !sc->dxgi_mode)
return VK_SUCCESS;
VkResult res = sd->real.ResetCommandBuffer(sc->barrier_cmds[0], 0);
if (res != VK_SUCCESS) { STEREO_ERR("ResetCommandBuffer failed: %d", res); return res; }
res = record_barrier_cmd(sd, sc);
if (res != VK_SUCCESS) { STEREO_ERR("record_barrier_cmd failed: %d", res); return res; }
VkPipelineStageFlags *stage_masks = NULL;
if (wait_sem_count > 0) {
stage_masks = malloc(wait_sem_count * sizeof(VkPipelineStageFlags));
if (!stage_masks) return VK_ERROR_OUT_OF_HOST_MEMORY;
for (uint32_t i = 0; i < wait_sem_count; i++)
stage_masks[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
}
res = sd->real.ResetFences(sd->real_device, 1, &sc->barrier_fences[0]);
if (res != VK_SUCCESS) { free(stage_masks); return res; }
VkSubmitInfo submit = {
.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
.waitSemaphoreCount = wait_sem_count,
.pWaitSemaphores = wait_sems,
.pWaitDstStageMask = stage_masks,
.commandBufferCount = 1,
.pCommandBuffers = &sc->barrier_cmds[0],
};
res = sd->real.QueueSubmit(queue, 1, &submit, sc->barrier_fences[0]);
free(stage_masks);
if (res != VK_SUCCESS) {
STEREO_ERR("Barrier submit failed: %d", res);
return res;
}
res = sd->real.WaitForFences(
sd->real_device, 1, &sc->barrier_fences[0], VK_TRUE, UINT64_MAX);
if (res != VK_SUCCESS) {
STEREO_ERR("Barrier fence wait failed: %d", res);
return res;
}
return dxgi_copy_and_present(sd, sc);
}
