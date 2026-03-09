#pragma once
/*
 * present_alt.h — Alternative stereo presentation modes
 *
 *   STEREO_PRESENT_DX9        — NvAPI + IDirect3D9Ex exclusive-fullscreen direct-mode
 *                                Works with NVIDIA 3D Vision via 3D Fix Manager
 *   STEREO_PRESENT_SBS        — Side-by-side (half-width, left | right)
 *   STEREO_PRESENT_TAB        — Top-and-bottom (half-height, top=left / bottom=right)
 *   STEREO_PRESENT_INTERLACED — Row-interleaved (even=left, odd=right)
 *
 * CPU staging flow (DX9 and compose modes):
 *   Vulkan layer-0 (left) + layer-1 (right) → VkBuffer via CopyImageToBuffer
 *   → D3D9 POOL_SYSTEMMEM surface lock / D3D11 UpdateSubresource
 *   → D3D9 Present / DXGI Present
 *
 * Hotkeys polled in QueuePresentKHR:
 *   Ctrl+F3 — decrease separation
 *   Ctrl+F4 — increase separation
 *   Ctrl+F5 — decrease convergence
 *   Ctrl+F6 — increase convergence
 *   Ctrl+F7 — save separation + convergence to local INI
 */

#include "stereo_icd.h"

/* ── CPU staging (Vulkan side) ───────────────────────────────────────────── */

/* Allocate VkBuffer + memory for both-eyes CPU readback.
 * On success, sets sc->cpu_buf/mem/map/pool/cmd/fence, sc->cpu_ok=true. */
VkResult alt_cpu_staging_init(StereoDevice *sd, StereoSwapchain *sc);
void     alt_cpu_staging_destroy(StereoDevice *sd, StereoSwapchain *sc);

/* Record + submit a command that copies both layers to sc->cpu_map,
 * then waits for completion.  After this returns, cpu_map[0..W*H*4-1] holds
 * left eye pixels and cpu_map[W*H*4..2*W*H*4-1] holds right eye pixels.
 * layout_in = current image layout (typically COLOR_ATTACHMENT_OPTIMAL).
 * The image is restored to layout_in afterwards.                          */
VkResult alt_cpu_readback(StereoDevice *sd, StereoSwapchain *sc,
                          VkQueue queue,
                          uint32_t wait_sem_count,
                          const VkSemaphore *wait_sems,
                          VkImageLayout layout_in);

/* ── Allocate a regular (non-external) 2-layer stereo image ─────────────── */
VkResult alt_alloc_stereo_image(StereoDevice *sd, StereoSwapchain *sc,
                                VkImage *out_image, VkDeviceMemory *out_mem);

/* ── DX9 direct-mode (3D Vision via 3D Fix Manager) ─────────────────────── */

/* Initialise D3D9Ex device + NvAPI stereo on sc->hwnd.
 * Must be called after dxgi_device_init (for NvAPI, which is already loaded).
 * Returns true on success; stores state in sd->dx9_*. */
bool     dx9_init(StereoDevice *sd, StereoSwapchain *sc);
void     dx9_destroy(StereoDevice *sd);
VkResult dx9_present(StereoDevice *sd, StereoSwapchain *sc,
                     VkQueue queue,
                     uint32_t wait_sem_count,
                     const VkSemaphore *wait_sems);

/* ── Compose modes (SBS / TAB / Interlaced) ─────────────────────────────── */

/* Initialise DXGI windowed swap chain for composed output on sc->hwnd.
 * Uses the existing sd->d3d11_dev / sd->d3d11_ctx. */
bool     compose_init(StereoDevice *sd, StereoSwapchain *sc);
void     compose_destroy(StereoDevice *sd);
VkResult compose_present(StereoDevice *sd, StereoSwapchain *sc,
                         VkQueue queue,
                         uint32_t wait_sem_count,
                         const VkSemaphore *wait_sems,
                         StereoPresentMode mode);

/* ── Hotkeys ─────────────────────────────────────────────────────────────── */

/* Poll Ctrl+F3..F7 and act.  Call once per QueuePresentKHR.
 * Adjusts sd->stereo.separation / convergence and saves to INI on F7. */
void hotkeys_poll(StereoDevice *sd);
