#ifdef __cplusplus
extern "C" {
#endif

bool nv3d_init(
    StereoDevice* sd,
    uint32_t width,
    uint32_t height);

void nv3d_destroy(
    StereoDevice* sd);

VkResult nv3d_present(
    StereoDevice* sd,
    StereoSwapchain* sc,
    VkQueue queue,
    uint32_t wait_sem_count,
    const VkSemaphore* wait_sems);

#ifdef __cplusplus
}
#endif