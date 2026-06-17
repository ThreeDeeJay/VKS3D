#pragma once

#include "stereo_icd.h"

bool nv3d_init(
    StereoDevice *sd,
    StereoSwapchain *sc);

void nv3d_destroy(
    StereoDevice *sd);

VkResult nv3d_present(
    StereoDevice *sd,
    StereoSwapchain *sc,
    VkQueue queue,
    uint32_t wait_sem_count,
    const VkSemaphore *wait_sems);