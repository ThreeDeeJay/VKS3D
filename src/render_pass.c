
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#define STEREO_VIEW_MASK 0x3u
#define STEREO_CORRELATION_MASK 0x3u
static VkResult create_patched_rp(StereoDevice *sd,
const VkRenderPassCreateInfo *pCI, const VkAllocationCallbacks *pA,
VkRenderPass *pRP)
{
bool needs_patch = false;
for (uint32_t i = 0; i < pCI->attachmentCount; i++)
if (pCI->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
{ needs_patch = true; break; }
if (!needs_patch)
return sd->real.CreateRenderPass(sd->real_device, pCI, pA, pRP);
VkAttachmentDescription *pa = malloc(pCI->attachmentCount * sizeof(*pa));
if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
for (uint32_t i = 0; i < pCI->attachmentCount; i++) {
pa[i] = pCI->pAttachments[i];
if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
VkRenderPassCreateInfo mod = *pCI; mod.pAttachments = pa;
VkResult res = sd->real.CreateRenderPass(sd->real_device, &mod, pA, pRP);
free(pa); return res;
}
static bool is_depth_only_renderpass(
const VkRenderPassCreateInfo *pCI)
{
if (!pCI || pCI->attachmentCount != 1)
return false;
VkFormat fmt = pCI->pAttachments[0].format;
switch (fmt) {
case VK_FORMAT_D16_UNORM:
case VK_FORMAT_X8_D24_UNORM_PACK32:
case VK_FORMAT_D32_SFLOAT:
case VK_FORMAT_S8_UINT:
case VK_FORMAT_D16_UNORM_S8_UINT:
case VK_FORMAT_D24_UNORM_S8_UINT:
case VK_FORMAT_D32_SFLOAT_S8_UINT:
return true;
default:
return false;
}
}
static bool is_shadow_renderpass(
const VkRenderPassCreateInfo *pCI)
{
if (!pCI || pCI->attachmentCount != 2)
return false;
const VkAttachmentDescription *color = &pCI->pAttachments[0];
const VkAttachmentDescription *depth = &pCI->pAttachments[1];
if (color->format != VK_FORMAT_R32_SFLOAT)
return false;
switch (depth->format) {
case VK_FORMAT_D16_UNORM:
case VK_FORMAT_X8_D24_UNORM_PACK32:
case VK_FORMAT_D32_SFLOAT:
case VK_FORMAT_S8_UINT:
case VK_FORMAT_D16_UNORM_S8_UINT:
case VK_FORMAT_D24_UNORM_S8_UINT:
case VK_FORMAT_D32_SFLOAT_S8_UINT:
return true;
default:
return false;
}
}
static bool find_shading_rate_attachment2(
const VkRenderPassCreateInfo2 *pCI,
uint32_t *attachment)
{
if (!pCI || !attachment)
return false;
*attachment = VK_ATTACHMENT_UNUSED;
#ifdef VK_KHR_fragment_shading_rate
for (uint32_t i = 0; i < pCI->subpassCount; i++) {
const VkBaseInStructure *pNext =
(const VkBaseInStructure *)pCI->pSubpasses[i].pNext;
while (pNext) {
if (pNext->sType ==
VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) {
const VkFragmentShadingRateAttachmentInfoKHR *fsr =
(const VkFragmentShadingRateAttachmentInfoKHR *)pNext;
if (fsr->pFragmentShadingRateAttachment) {
*attachment =
fsr->pFragmentShadingRateAttachment->attachment;
return *attachment != VK_ATTACHMENT_UNUSED;
}
}
pNext = pNext->pNext;
}
}
#endif
return false;
}
static VkResult create_mv_rp(StereoDevice *sd,
const VkRenderPassCreateInfo *pCI, const VkAllocationCallbacks *pA,
VkRenderPass *pRP)
{
VkAttachmentDescription *pa = NULL;
if (pCI->attachmentCount > 0) {
pa = malloc(pCI->attachmentCount * sizeof(*pa));
if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
for (uint32_t i = 0; i < pCI->attachmentCount; i++) {
pa[i] = pCI->pAttachments[i];
if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
}
uint32_t sc = pCI->subpassCount;
uint32_t *vm = malloc(sc * sizeof(uint32_t));
uint32_t *cm = malloc(sc * sizeof(uint32_t));
int32_t *vo = pCI->dependencyCount ? calloc(pCI->dependencyCount, sizeof(*vo)) : NULL;
if (!vm || !cm || (pCI->dependencyCount && !vo)) {
free(pa); free(vm); free(cm); free(vo); return VK_ERROR_OUT_OF_HOST_MEMORY;
}
for (uint32_t i = 0; i < sc; i++) { vm[i] = 0x00000003; cm[i] = STEREO_CORRELATION_MASK; }
VkRenderPassMultiviewCreateInfo mv = {
.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
.pNext = NULL, .subpassCount = sc, .pViewMasks = vm,
.dependencyCount = pCI->dependencyCount, .pViewOffsets = vo,
.correlationMaskCount = 0, .pCorrelationMasks = NULL,
};
VkRenderPassCreateInfo mod = *pCI;
if (pa) mod.pAttachments = pa;
const VkBaseInStructure *chain = (const VkBaseInStructure *)pCI->pNext;
while (chain)
{
if (chain->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
{
free(pa); free(vm); free(cm); free(vo);
return VK_INCOMPLETE;
}
chain = chain->pNext;
}
mv.pNext = pCI->pNext;
mod.pNext = &mv;
VkResult res = sd->real.CreateRenderPass(sd->real_device, &mod, pA, pRP);
free(pa); free(vm); free(cm); free(vo);
return res;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRenderPass(
VkDevice device,
const VkRenderPassCreateInfo *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkRenderPass *pRenderPass)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
if (pCreateInfo) {
for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
const VkAttachmentDescription *a = &pCreateInfo->pAttachments[i];
}
for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
const VkSubpassDescription *s = &pCreateInfo->pSubpasses[i];
for (uint32_t j = 0; j < s->colorAttachmentCount; j++) {
}
if (s->pDepthStencilAttachment) {
}
}
}
VkResult res = create_patched_rp(sd, pCreateInfo, pAllocator, pRenderPass);
if (res != VK_SUCCESS)
return res;
if (sd->render_pass_count >= MAX_RENDER_PASSES)
return VK_SUCCESS;
CHECK_ARRAY_COUNT(sd->render_pass_count, MAX_RENDER_PASSES, "render_pass_count");
StereoRenderPassInfo *rpi =
&sd->render_passes[sd->render_pass_count++];
rpi->handle = *pRenderPass;
rpi->mv_handle = VK_NULL_HANDLE;
rpi->has_multiview = false;
rpi->view_mask = 0;
rpi->subpass_count = pCreateInfo->subpassCount;
rpi->shading_rate_attachment = VK_ATTACHMENT_UNUSED;
rpi->has_shading_rate_attachment = false;
bool depth_only = is_depth_only_renderpass(pCreateInfo);
bool shadow_pass = is_shadow_renderpass(pCreateInfo);
bool present_pass = false;
for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
if (pCreateInfo->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
{
present_pass = true;
break;
}
}
bool mv_eligible =
sd->stereo.enabled &&
sd->stereo.multiview &&
pCreateInfo->attachmentCount > 0 &&
!depth_only &&
!shadow_pass;
for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
}
if (!mv_eligible)
{
if (shadow_pass && pCreateInfo->pAttachments)
{
for (uint32_t a = 0; a < pCreateInfo->attachmentCount; a++)
{
const VkAttachmentDescription *ad = &pCreateInfo->pAttachments[a];
}
}
if (shadow_pass && pCreateInfo->pSubpasses)
{
for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++)
{
const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[s];
}
}
return VK_SUCCESS;
}
if (sd->render_pass_count > 1)
{
for (uint32_t i = 0; i < sd->render_pass_count - 1; i++)
{
StereoRenderPassInfo *ex = &sd->render_passes[i];
if (ex->handle == *pRenderPass && ex->mv_handle)
{
rpi->mv_handle = ex->mv_handle;
rpi->has_multiview = true;
rpi->view_mask = STEREO_VIEW_MASK;
rpi->shading_rate_attachment = ex->shading_rate_attachment;
rpi->has_shading_rate_attachment =
ex->has_shading_rate_attachment;
return VK_SUCCESS;
}
}
}
VkRenderPass mv = VK_NULL_HANDLE;
if (create_mv_rp(sd, pCreateInfo, pAllocator, &mv) == VK_SUCCESS && mv)
{
rpi->mv_handle = mv;
rpi->has_multiview = true;
rpi->view_mask = STEREO_VIEW_MASK;
sd->multiview_pass_exists = true;
}
else
{
STEREO_ERR("RP_MV_CREATE_FAILED rp=%p result_or_handle_invalid=1 attachments=%u subpasses=%u",
(void*)*pRenderPass,
pCreateInfo->attachmentCount,
pCreateInfo->subpassCount);
}
return VK_SUCCESS;
}
#ifdef VK_KHR_create_renderpass2
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRenderPass2KHR(
VkDevice device,
const VkRenderPassCreateInfo2 *pCreateInfo,
const VkAllocationCallbacks *pAllocator,
VkRenderPass *pRenderPass)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
if (!sd->real.CreateRenderPass2KHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
uint32_t ac = pCreateInfo->attachmentCount, sc = pCreateInfo->subpassCount;
VkAttachmentDescription2 *pa = NULL;
if (ac > 0) {
pa = malloc(ac * sizeof(*pa));
if (!pa) return VK_ERROR_OUT_OF_HOST_MEMORY;
for (uint32_t i = 0; i < ac; i++) {
pa[i] = pCreateInfo->pAttachments[i];
if (pa[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
pa[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
}
VkRenderPassCreateInfo2 orig = *pCreateInfo;
if (pa) orig.pAttachments = pa;
VkResult res = sd->real.CreateRenderPass2KHR(sd->real_device, &orig, pAllocator, pRenderPass);
free(pa);
if (res != VK_SUCCESS) return res;
if (sd->render_pass_count >= MAX_RENDER_PASSES) return VK_SUCCESS;
CHECK_ARRAY_COUNT(sd->render_pass_count, MAX_RENDER_PASSES, "render_pass_count");
StereoRenderPassInfo *rpi = &sd->render_passes[sd->render_pass_count++];
rpi->handle = *pRenderPass;
rpi->mv_handle = VK_NULL_HANDLE;
rpi->has_multiview = false;
rpi->view_mask = 0;
rpi->subpass_count = sc;
rpi->shading_rate_attachment = VK_ATTACHMENT_UNUSED;
rpi->has_shading_rate_attachment =
find_shading_rate_attachment2(
pCreateInfo,
&rpi->shading_rate_attachment);
if (sd->stereo.enabled && sd->stereo.multiview) {
VkAttachmentDescription2 *pa2 = NULL;
if (ac > 0) {
pa2 = malloc(ac * sizeof(*pa2));
if (!pa2) return VK_SUCCESS;
for (uint32_t i = 0; i < ac; i++) {
pa2[i] = pCreateInfo->pAttachments[i];
if (pa2[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
pa2[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
}
VkSubpassDescription2 *subs = malloc(sc * sizeof(*subs));
uint32_t *corr = malloc(sc * sizeof(*corr));
if (subs && corr) {
for (uint32_t i = 0; i < sc; i++) {
subs[i] = pCreateInfo->pSubpasses[i];
subs[i].viewMask = STEREO_VIEW_MASK;
corr[i] = STEREO_CORRELATION_MASK;
}
VkRenderPassCreateInfo2 mv2 = *pCreateInfo;
if (pa2) mv2.pAttachments = pa2;
mv2.pSubpasses = subs;
mv2.correlatedViewMaskCount = 0;
mv2.pCorrelatedViewMasks = NULL;
VkRenderPass mv = VK_NULL_HANDLE;
VkResult mv2_res = sd->real.CreateRenderPass2KHR(sd->real_device, &mv2, pAllocator, &mv);
if (mv2_res == VK_SUCCESS && mv) {
rpi->mv_handle = mv; rpi->has_multiview = true;
rpi->view_mask = STEREO_VIEW_MASK;
sd->multiview_pass_exists = true;
}
}
free(pa2); free(subs); free(corr);
}
return VK_SUCCESS;
}
#endif