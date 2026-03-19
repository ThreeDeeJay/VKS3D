#pragma once
/* dxgi_output.h — D3D11/DXGI stereo output interface (external memory path) */

/* Forward declarations (full types in stereo_icd.h) */
typedef struct StereoDevice    StereoDevice;
typedef struct StereoSwapchain StereoSwapchain;

/* ── Device-level D3D11 init / teardown ─────────────────────────────────── */
bool dxgi_device_init(StereoDevice *sd);
void dxgi_device_destroy(StereoDevice *sd);

/* ── Per-swapchain DXGI swap chain ──────────────────────────────────────── */
bool dxgi_sc_create(StereoDevice *sd, StereoSwapchain *sc, HANDLE *out_nt_handle);
void dxgi_sc_destroy(StereoSwapchain *sc);

/* ── Shared Texture2DArray[2] — Vulkan external memory import source ────── *
 *
 * Creates one D3D11 Texture2DArray[2] with D3D11_RESOURCE_MISC_SHARED_NTHANDLE.
 * Stores the ID3D11Texture2D* in sc->shared_d3d11_tex.
 * Returns an NT HANDLE the caller must pass to vkAllocateMemory
 * (VkImportMemoryWin32HandleInfoKHR).  NT handles are consumed on import;
 * do NOT CloseHandle() until after vkAllocateMemory returns.
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── Per-frame GPU copy + DXGI present ──────────────────────────────────── *
 *
 * Called AFTER the CPU has waited for the Vulkan render fence (so the shared
 * D3D11 texture is fully rendered).  Performs a GPU-side
 * CopySubresourceRegion from sc->shared_d3d11_tex (slices 0 and 1) into the
 * DXGI back buffer and calls IDXGISwapChain::Present(1, 0).
 *
 * No CPU pixel copy.  No PCIe round-trip.
 * ─────────────────────────────────────────────────────────────────────────── */
VkResult dxgi_copy_and_present(StereoDevice *sd, StereoSwapchain *sc);
