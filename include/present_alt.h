#pragma once
/*
 * present_alt.h — Alternative stereo presentation modes
 *
 *   STEREO_PRESENT_DX9        — NvAPI + IDirect3D9Ex exclusive-fullscreen direct-mode
 *   STEREO_PRESENT_SBS        — Side-by-side (half-width, left | right)  [GPU blit]
 *   STEREO_PRESENT_TAB        — Top-and-bottom (half-height)              [GPU blit]
 *   STEREO_PRESENT_INTERLACED — Row-interleaved                           [CPU fallback]
 *
 * GPU blit flow (SBS / TAB):
 *   stereo_images[0] layer0=left, layer1=right (rendered by app via multiview)
 *   → CmdBlitImage × 2 into real VkSwapchainKHR image (no PCIe round-trip)
 *   → QueuePresentKHR
 *   Backpressure: WaitForFences(barrier_fences[0]) at next AcquireNextImageKHR
 *
 * CPU path (DX9 only):
 *   alt_cpu_readback: GPU → VkBuffer → CPU map → D3D9 surface → Present
 *
 * Hotkeys polled in QueuePresentKHR:
 *   Ctrl+F3 — decrease separation    Ctrl+F4 — increase separation
 *   Ctrl+F5 — decrease convergence   Ctrl+F6 — increase convergence
 *   Ctrl+F7 — save to local INI
 */

#include "stereo_icd.h"

/* ── CPU staging (Vulkan → host buffer, used by DX9 path only) ──────────── */
VkResult alt_cpu_staging_init(StereoDevice *sd, StereoSwapchain *sc);
void     alt_cpu_staging_destroy(StereoDevice *sd, StereoSwapchain *sc);
VkResult alt_cpu_readback(StereoDevice *sd, StereoSwapchain *sc,
                          VkQueue queue,
                          uint32_t wait_sem_count,
                          const VkSemaphore *wait_sems,
                          VkImageLayout layout_in);

/* ── Allocate a regular (non-external) 2-layer stereo render target ──────── */
VkResult alt_alloc_stereo_image(StereoDevice *sd, StereoSwapchain *sc,
                                VkImage *out_image, VkDeviceMemory *out_mem);

/* ── DX9 direct-mode (3D Vision via NvAPI) ───────────────────────────────── */
bool     dx9_init(StereoDevice *sd, StereoSwapchain *sc);
void     dx9_destroy(StereoDevice *sd);
VkResult dx9_present(StereoDevice *sd, StereoSwapchain *sc,
                     VkQueue queue,
                     uint32_t wait_sem_count,
                     const VkSemaphore *wait_sems);

/* ── GPU blit compose (SBS / TAB) ────────────────────────────────────────── *
 *
 * Creates a real VkSwapchainKHR on the app's surface and blits both eye
 * layers into it on the GPU.  No CPU readback, no PCIe round-trip, no GDI.
 * ~50× faster than the old CPU compose path.
 *
 * gpu_compose_sc_init  — called from stereo_CreateSwapchainKHR; sets
 *                         sc->real_swapchain, sc->comp_sc_images,
 *                         sc->comp_acquire_sem, sc->comp_blit_done_sem.
 * gpu_compose_sc_destroy — frees semaphores and comp_sc_images array;
 *                          sc->real_swapchain is destroyed by the caller.
 * gpu_compose_present  — per-frame: AcquireNextImage → CmdBlitImage ×2
 *                         → QueueSubmit → QueuePresentKHR.             */
bool     gpu_compose_sc_init(StereoDevice *sd, StereoSwapchain *sc,
                             VkSurfaceKHR surface);
void     gpu_compose_sc_destroy(StereoDevice *sd, StereoSwapchain *sc);
VkResult gpu_compose_present(StereoDevice *sd, StereoSwapchain *sc,
                             VkQueue queue,
                             uint32_t wait_sem_count,
                             const VkSemaphore *wait_sems);

/* ── CPU compose (SBS / TAB / Interlaced, GDI fallback) ─────────────────── */
bool     compose_init(StereoDevice *sd, StereoSwapchain *sc);
void     compose_destroy(StereoDevice *sd);
VkResult compose_present(StereoDevice *sd, StereoSwapchain *sc,
                         VkQueue queue,
                         uint32_t wait_sem_count,
                         const VkSemaphore *wait_sems,
                         StereoPresentMode mode);

/* ── Hotkeys ─────────────────────────────────────────────────────────────── */
void hotkeys_poll(StereoDevice *sd);