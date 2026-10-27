#pragma once
#include "stereo_icd.h"
VkResult alt_cpu_staging_init(StereoDevice *sd, StereoSwapchain *sc);
void alt_cpu_staging_destroy(StereoDevice *sd, StereoSwapchain *sc);
VkResult alt_cpu_readback(StereoDevice *sd, StereoSwapchain *sc,
VkQueue queue,
uint32_t wait_sem_count,
const VkSemaphore *wait_sems,
VkImageLayout layout_in);
VkResult alt_alloc_stereo_image(StereoDevice *sd, StereoSwapchain *sc,
VkImage *out_image, VkDeviceMemory *out_mem);
bool dx9_init(StereoDevice *sd, StereoSwapchain *sc);
void dx9_destroy(StereoDevice *sd);
VkResult dx9_present(StereoDevice *sd, StereoSwapchain *sc,
VkQueue queue,
uint32_t wait_sem_count,
const VkSemaphore *wait_sems);
bool gpu_compose_sc_init(StereoDevice *sd, StereoSwapchain *sc,
VkSurfaceKHR surface);
void gpu_compose_sc_destroy(StereoDevice *sd, StereoSwapchain *sc);
VkResult gpu_compose_present(StereoDevice *sd, StereoSwapchain *sc,
VkQueue queue,
uint32_t wait_sem_count,
const VkSemaphore *wait_sems);
bool compose_init(StereoDevice *sd, StereoSwapchain *sc);
void compose_destroy(StereoDevice *sd);
VkResult compose_present(StereoDevice *sd, StereoSwapchain *sc,
VkQueue queue,
uint32_t wait_sem_count,
const VkSemaphore *wait_sems,
StereoPresentMode mode);
void hotkeys_poll(StereoDevice *sd);