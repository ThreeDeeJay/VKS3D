#pragma once
/* dxgi_output.h — D3D11/DXGI stereo output interface */

/* Forward declarations (full types in stereo_icd.h) */
typedef struct StereoDevice   StereoDevice;
typedef struct StereoSwapchain StereoSwapchain;

/* ── Device-level D3D11 init / teardown ─────────────────────────────────── */
bool dxgi_device_init(StereoDevice *sd);
void dxgi_device_destroy(StereoDevice *sd);

/* ── Per-swapchain DXGI swap chain + staging textures ───────────────────── */
bool dxgi_sc_create(StereoDevice *sd, StereoSwapchain *sc);
bool dxgi_tex_create(StereoDevice *sd, StereoSwapchain *sc);
void dxgi_sc_destroy(StereoSwapchain *sc);

/* ── Per-frame composite + DXGI present ─────────────────────────────────── */
/*
 * stage_left:  pointer to W×H×4 bytes for left  eye (Vulkan layer 0), CPU-visible
 * stage_right: pointer to W×H×4 bytes for right eye (Vulkan layer 1), CPU-visible
 */
VkResult dxgi_present_frame(StereoDevice *sd, StereoSwapchain *sc,
                             const void *stage_left, const void *stage_right);
